/*
 * bass_detector.cpp — Implémentation du détecteur de basses
 *
 * Pipeline exécuté à l'arrivée de chaque paquet audio (~10ms) :
 *
 * 1. FILTRAGE : Butterworth 4ème ordre passe-bas à la fréquence de cutoff,
 *    appliqué en flux. L'état du filtre persiste d'un paquet à l'autre :
 *    chaque échantillon n'est filtré qu'une seule fois.
 *
 * 2. DÉCIMATION : On garde 1 sample sur ~120 → 48kHz → ~400Hz.
 *    Les samples décimés alimentent une fenêtre glissante.
 *
 * 3. Dès qu'un nouveau "hop" (~10ms) est arrivé, on analyse la fenêtre
 *    la plus récente (300ms par défaut) :
 *
 *    a. FFT : Fenêtre de Hann + zero-padding + FFTW r2c (réel vers complexe).
 *       Le zero-padding garde une grille fine (~0.4Hz/bin) malgré la fenêtre courte.
 *
 *    b. PEAK DETECTION : Recherche du bin de magnitude maximale dans [15Hz, cutoff].
 *       Interpolation parabolique pour précision sub-bin.
 *
 *    c. PITCH DETECTION : Autocorrélation McLeod NSDF sur les ~200 dernières ms
 *       du signal décimé (non fenêtré, pour ne pas biaiser la périodicité).
 *
 * Latence : le pic reflète le son d'environ (fenêtre / 2) en arrière.
 * Avec 300ms → ~150ms, contre ~1.3s avec l'ancienne fenêtre fixe de 2.56s.
 */
#include "bass_detector.h"
#include <cmath>
#include <algorithm>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

BassDetector::BassDetector() {}

BassDetector::~BassDetector() {
    // Libération des buffers FFTW
    if (m_fftPlan) fftwf_destroy_plan(m_fftPlan);
    if (m_fftIn)   fftwf_free(m_fftIn);
    if (m_fftOut)  fftwf_free(m_fftOut);
}

void BassDetector::init(uint32_t sourceSampleRate, float bassCutoffHz) {
    std::lock_guard<std::mutex> lock(m_processMutex);
    m_sourceSampleRate = sourceSampleRate;
    m_cutoffHz = bassCutoffHz;
    configure();
}

void BassDetector::configure() {
    // === Calcul du facteur de décimation ===
    // On cible un sample rate effectif = 4× le cutoff (bien au-dessus de Nyquist)
    // Ex: cutoff 100Hz → cible 400Hz → décimation 48000/400 = 120
    float targetRate = m_cutoffHz * 4.0f;
    m_decimFactor = std::max(1, static_cast<int>(m_sourceSampleRate / targetRate));
    m_effectiveSampleRate = m_sourceSampleRate / m_decimFactor;

    // === Design du filtre passe-bas ===
    // Le cutoff du filtre doit être en dessous de Nyquist du sample rate effectif
    // pour éviter l'aliasing lors de la décimation
    float filterCutoff = std::min(m_cutoffHz, m_effectiveSampleRate * 0.45f);
    m_lowpass.design(filterCutoff, static_cast<float>(m_sourceSampleRate));

    // === Fenêtre d'analyse ===
    // Sa durée fixe le compromis latence / résolution :
    //   - latence ≈ fenêtre / 2 (centre de la fenêtre de Hann)
    //   - deux fréquences ne sont séparables que si elles sont écartées de ≳ 2 / fenêtre
    // Ex: 300ms à 400Hz → 120 samples, latence ~150ms, séparation ~7Hz
    m_windowLen = std::max<size_t>(8,
        static_cast<size_t>(m_windowMs / 1000.0f * m_effectiveSampleRate + 0.5f));

    // === Dimensionnement de la FFT (zero-padding) ===
    // La FFT est plus grande que la fenêtre : le reste est complété par des zéros.
    // Ça n'ajoute pas de vraie résolution, mais ça interpole le spectre sur une
    // grille fine (≤ 0.5Hz/bin) → courbe lisse et pic mieux localisé.
    // Arrondi à la puissance de 2 supérieure (requis pour FFTW optimal)
    size_t targetSize = std::max(m_windowLen, static_cast<size_t>(m_effectiveSampleRate) * 2);
    m_fftSize = 1;
    while (m_fftSize < targetSize) m_fftSize <<= 1;

    // === Hop : une analyse toutes les ~10ms (≈ un paquet WASAPI) ===
    m_hopLen = std::max<size_t>(1,
        static_cast<size_t>(m_effectiveSampleRate * 0.010f + 0.5f));

    // === Fenêtre de l'autocorrélation ===
    // La NSDF a besoin d'au moins 2 périodes de la fréquence la plus basse (20Hz).
    // On en prend ~4 (200ms) : assez pour être fiable, assez court pour être réactif.
    m_pitchDetector.init(m_effectiveSampleRate, 20.0f, m_cutoffHz + 20.0f);
    size_t maxLag = static_cast<size_t>(m_effectiveSampleRate / 20.0f);
    m_nsdfLen = std::max(2 * maxLag + 1, std::min(m_windowLen, 4 * maxLag));

    // === Allocation des buffers FFTW ===
    // fftwf_alloc_* garantit l'alignement mémoire optimal (SIMD)
    if (m_fftPlan) fftwf_destroy_plan(m_fftPlan);
    if (m_fftIn)   fftwf_free(m_fftIn);
    if (m_fftOut)  fftwf_free(m_fftOut);

    m_fftIn  = fftwf_alloc_real(m_fftSize);                // N réels en entrée
    m_fftOut = fftwf_alloc_complex(m_fftSize / 2 + 1);     // N/2+1 complexes en sortie
    // FFTW_ESTIMATE : le plan est créé instantanément (pas de benchmark),
    // ce qui garde les changements de réglage fluides. À ces tailles,
    // la FFT ne prend que quelques microsecondes de toute façon.
    m_fftPlan = fftwf_plan_dft_r2c_1d(
        static_cast<int>(m_fftSize), m_fftIn, m_fftOut, FFTW_ESTIMATE
    );

    // === Reset du traitement en flux ===
    m_decBuffer.clear();
    m_decBuffer.reserve(std::max(m_windowLen, m_nsdfLen) * 2);
    m_decimPhase  = 0;
    m_newSamples  = 0;
    m_recentHead  = 0;
    m_recentCount = 0;

    // Démarrage du chrono (une seule fois)
    if (!m_started) {
        m_startTime = std::chrono::steady_clock::now();
        m_started = true;
    }

    std::cout << "[BassDetector] Source: " << m_sourceSampleRate << " Hz\n";
    std::cout << "[BassDetector] Decimation: x" << m_decimFactor
              << " -> Effective: " << m_effectiveSampleRate << " Hz\n";
    std::cout << "[BassDetector] Window: " << m_windowMs << " ms (" << m_windowLen
              << " samples), hop: " << m_hopLen << " samples\n";
    std::cout << "[BassDetector] FFT size: " << m_fftSize
              << " (grid: " << (float)m_effectiveSampleRate / m_fftSize << " Hz/bin)\n";
    std::cout << "[BassDetector] Cutoff: " << m_cutoffHz << " Hz\n";
}

void BassDetector::feedSamples(const std::vector<float>& samples) {
    std::lock_guard<std::mutex> lock(m_processMutex);
    if (!m_fftPlan) return; // init() pas encore appelé

    // --- 1. Filtrage passe-bas en flux (au sample rate source, ex: 48kHz) ---
    // Le filtre Butterworth atténue tout au-dessus du cutoff.
    // IMPORTANT : filtrer AVANT de décimer, sinon aliasing !
    m_filtered.assign(samples.begin(), samples.end());
    m_lowpass.process(m_filtered);

    // --- 2. Décimation en flux vers la fenêtre glissante ---
    size_t before = m_decBuffer.size();
    DSP::decimate(m_filtered, m_decimFactor, m_decimPhase, m_decBuffer);
    m_newSamples += m_decBuffer.size() - before;

    // On ne garde que ce qui sert à la prochaine analyse
    size_t keep = std::max(m_windowLen, m_nsdfLen);
    if (m_decBuffer.size() > keep) {
        m_decBuffer.erase(m_decBuffer.begin(),
                          m_decBuffer.end() - static_cast<std::ptrdiff_t>(keep));
    }

    // --- 3. Analyse dès qu'un hop complet est arrivé ---
    // Une seule analyse par paquet, sur la fenêtre la plus récente :
    // des analyses intermédiaires ne seraient jamais affichées.
    if (m_decBuffer.size() >= keep && m_newSamples >= m_hopLen) {
        processBuffer();
        m_newSamples = 0;
    }
}

void BassDetector::processBuffer() {
    // --- 1. Fenêtrage de Hann sur les m_windowLen derniers samples ---
    std::vector<float> windowed(m_decBuffer.end() - static_cast<std::ptrdiff_t>(m_windowLen),
                                m_decBuffer.end());
    DSP::applyHannWindow(windowed);

    // --- 2. FFT avec zero-padding jusqu'à m_fftSize ---
    std::copy(windowed.begin(), windowed.end(), m_fftIn);
    std::fill(m_fftIn + m_windowLen, m_fftIn + m_fftSize, 0.0f);
    fftwf_execute(m_fftPlan);

    // --- 3. Analyse du spectre ---
    size_t numBins = m_fftSize / 2 + 1;
    float freqPerBin = static_cast<float>(m_effectiveSampleRate) / static_cast<float>(m_fftSize);

    // Plage de bins à analyser
    size_t maxBin = std::min(
        static_cast<size_t>(m_cutoffHz / freqPerBin) + 1,
        numBins - 1
    );
    // On skip les bins < 15Hz (infrasons, bruit de fond, rumble de platine)
    size_t minBin = std::max(size_t(1), static_cast<size_t>(15.0f / freqPerBin));

    // Construction du spectre pour la GUI
    SpectrumData spectrum;
    spectrum.freqPerBin = freqPerBin;
    spectrum.numBins = maxBin + 1;
    spectrum.cutoffHz = m_cutoffHz;
    spectrum.magnitudesDb.resize(maxBin + 1);
    spectrum.frequencies.resize(maxBin + 1);

    // Facteur de normalisation FFT (pour obtenir des dB absolus).
    // On normalise par la taille de la fenêtre, pas de la FFT : le zero-padding
    // ne change pas l'amplitude des pics → mêmes dB qu'avant pour un même signal.
    float refMag = static_cast<float>(m_windowLen);

    float peakMag = 0.0f;
    size_t peakBin = 0;
    float totalEnergy = 0.0f;

    for (size_t i = 0; i <= maxBin; i++) {
        float re = m_fftOut[i][0]; // Partie réelle
        float im = m_fftOut[i][1]; // Partie imaginaire
        float mag = sqrtf(re * re + im * im); // Module = √(re² + im²)

        spectrum.frequencies[i] = i * freqPerBin;
        spectrum.magnitudesDb[i] = 20.0f * log10f(std::max(mag / refMag, 1e-10f));

        // Recherche du pic dans la bande [15Hz, cutoff]
        if (i >= minBin) {
            totalEnergy += mag * mag;
            if (mag > peakMag) {
                peakMag = mag;
                peakBin = i;
            }
        }
    }

    // --- 4. Interpolation parabolique du pic FFT ---
    // On ajuste une parabole sur les 3 bins (gauche, centre, droite) autour du pic
    // pour estimer la vraie position du maximum avec précision sub-bin.
    //
    //   δ = 0.5 × (magR - magL) / (2·magC - magL - magR)
    //   freq = (peakBin + δ) × freqPerBin
    float peakFreq = peakBin * freqPerBin;
    if (peakBin > minBin && peakBin < maxBin) {
        float magL = sqrtf(m_fftOut[peakBin-1][0]*m_fftOut[peakBin-1][0] +
                          m_fftOut[peakBin-1][1]*m_fftOut[peakBin-1][1]);
        float magR = sqrtf(m_fftOut[peakBin+1][0]*m_fftOut[peakBin+1][0] +
                          m_fftOut[peakBin+1][1]*m_fftOut[peakBin+1][1]);
        float delta = 0.5f * (magR - magL) / (2.0f * peakMag - magL - magR);
        peakFreq = (peakBin + delta) * freqPerBin;
    }

    // Conversion en dB
    float peakDb = 20.0f * log10f(std::max(peakMag / refMag, 1e-10f));
    // RMS = énergie moyenne par bin (totalEnergy / nombre de bins dans la bande basse)
    size_t numBassBins = (maxBin >= minBin) ? (maxBin - minBin + 1) : 1;
    float rmsDb  = 10.0f * log10f(std::max(totalEnergy / numBassBins / (refMag * refMag), 1e-20f));
    bool  valid  = (peakMag > 0.001f * refMag); // Seuil de détection ≈ -60dB

    // --- 5. Fréquence stable : médiane des derniers pics valides ---
    // Les analyses arrivent toutes les ~10ms : sans ça, le chiffre affiché
    // scintillerait. La médiane rejette les sauts isolés sans "glisser"
    // entre deux notes comme le ferait une moyenne.
    float stableFreq = peakFreq;
    if (valid) {
        m_recentPeaks[m_recentHead] = peakFreq;
        m_recentHead = (m_recentHead + 1) % STABLE_COUNT;
        if (m_recentCount < STABLE_COUNT) m_recentCount++;

        std::array<float, STABLE_COUNT> sorted = m_recentPeaks;
        std::sort(sorted.begin(), sorted.begin() + m_recentCount);
        stableFreq = sorted[m_recentCount / 2];
    } else {
        // Silence → on repart de zéro, pour ne pas mélanger avec la note suivante
        m_recentHead  = 0;
        m_recentCount = 0;
    }

    // Timestamp
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_startTime).count();

    // --- 6. Détection de pitch par autocorrélation ---
    // Sur les m_nsdfLen derniers samples NON fenêtrés : la fenêtre de Hann
    // atténue les bords et fausserait la détection de périodicité.
    std::vector<float> pitchInput(m_decBuffer.end() - static_cast<std::ptrdiff_t>(m_nsdfLen),
                                  m_decBuffer.end());
    PitchInfo pitchResult = m_pitchDetector.detect(pitchInput);

    // --- 7. Assemblage du résultat ---
    BassInfo info;
    info.peakFrequency   = peakFreq;
    info.stableFrequency = stableFreq;
    info.peakMagnitude   = peakDb;
    info.rmsLevel        = rmsDb;
    info.valid           = valid;
    info.pitch           = pitchResult;
    info.timestamp       = elapsed;

    // Stockage thread-safe des résultats
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_latestResult = info;
        m_latestSpectrum = std::move(spectrum);
    }

    addHistoryEntry(info);
}

void BassDetector::addHistoryEntry(const BassInfo& info) {
    // Une entrée toutes les 50ms au plus (les analyses arrivent toutes les ~10ms) :
    // les 600 entrées couvrent ainsi ~30 secondes.
    if (info.timestamp - m_lastHistoryTs < 0.05) return;
    m_lastHistoryTs = info.timestamp;

    HistoryEntry entry;
    entry.timestamp     = info.timestamp;
    entry.peakFrequency = info.peakFrequency;
    entry.peakMagnitude = info.peakMagnitude;
    entry.valid         = info.valid;

    if (info.pitch.valid) {
        entry.pitchFrequency  = info.pitch.frequency;
        entry.pitchConfidence = info.pitch.confidence;
        entry.noteName        = info.pitch.noteName;
    }

    // Écriture dans le ring buffer
    std::lock_guard<std::mutex> lock(m_historyMutex);
    m_history[m_historyHead] = entry;
    m_historyHead = (m_historyHead + 1) % HISTORY_SIZE;
    if (m_historyCount < HISTORY_SIZE) m_historyCount++;
}

BassInfo BassDetector::getLatestResult() {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_latestResult;
}

SpectrumData BassDetector::getSpectrum() {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    return m_latestSpectrum;
}

std::vector<HistoryEntry> BassDetector::getHistory() {
    std::lock_guard<std::mutex> lock(m_historyMutex);
    std::vector<HistoryEntry> result;
    result.reserve(m_historyCount);

    // Quand le buffer n'est pas plein, start=0 et modulo n'a pas d'effet.
    // Quand il est plein, on lit depuis m_historyHead (le plus ancien).
    size_t start = (m_historyCount < HISTORY_SIZE) ? 0 : m_historyHead;
    for (size_t i = 0; i < m_historyCount; i++) {
        result.push_back(m_history[(start + i) % HISTORY_SIZE]);
    }

    return result;
}

void BassDetector::setCutoff(float hz) {
    // Changer le cutoff nécessite de recalculer tout : décimation, filtre, FFT.
    // Le mutex garantit que le thread audio n'est pas en pleine analyse.
    std::lock_guard<std::mutex> lock(m_processMutex);
    m_cutoffHz = hz;
    configure();
}

void BassDetector::setWindowMs(float ms) {
    std::lock_guard<std::mutex> lock(m_processMutex);
    m_windowMs = ms;
    configure();
}
