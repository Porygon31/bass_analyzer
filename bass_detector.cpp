/*
 * bass_detector.cpp — Implémentation du détecteur de basses
 *
 * Pipeline complet exécuté à chaque analyse :
 *
 * 1. ACCUMULATION : Les samples bruts (48kHz) arrivent par morceaux de ~480
 *    (10ms). On les accumule jusqu'à avoir assez pour une fenêtre FFT complète.
 *
 * 2. FILTRAGE : Butterworth 4ème ordre passe-bas à la fréquence de cutoff.
 *    Ça atténue tout ce qui est au-dessus (voix, aigus, etc.)
 *
 * 3. DÉCIMATION : On garde 1 sample sur ~120 → 48kHz → ~400Hz.
 *    La FFT n'a plus que ~800 points à traiter au lieu de ~96000.
 *
 * 4. FFT : Fenêtre de Hann + FFTW r2c (réel vers complexe).
 *    On obtient le spectre avec une résolution de ~0.5Hz.
 *
 * 5. PEAK DETECTION : Recherche du bin de magnitude maximale dans [15Hz, cutoff].
 *    Interpolation parabolique pour précision sub-bin.
 *
 * 6. PITCH DETECTION : Autocorrélation McLeod NSDF sur le signal décimé
 *    (non fenêtré, pour ne pas biaiser la périodicité).
 *
 * 7. OVERLAP : Les analyses se font avec 50% de recouvrement pour un suivi
 *    temporel plus fluide.
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

void BassDetector::init(uint32_t sourceSampleRate, float bassCutoffHz, size_t fftSize) {
    m_sourceSampleRate = sourceSampleRate;
    m_cutoffHz = bassCutoffHz;
    m_userFftSize = fftSize;

    // === Calcul du facteur de décimation ===
    // On cible un sample rate effectif = 4× le cutoff (bien au-dessus de Nyquist)
    // Ex: cutoff 100Hz → cible 400Hz → décimation 48000/400 = 120
    float targetRate = m_cutoffHz * 4.0f;
    m_decimFactor = std::max(1, static_cast<int>(sourceSampleRate / targetRate));
    m_effectiveSampleRate = sourceSampleRate / m_decimFactor;

    // === Design du filtre passe-bas ===
    // Le cutoff du filtre doit être en dessous de Nyquist du sample rate effectif
    // pour éviter l'aliasing lors de la décimation
    float filterCutoff = std::min(m_cutoffHz, m_effectiveSampleRate * 0.45f);
    m_lowpass.design(filterCutoff, static_cast<float>(sourceSampleRate));

    // === Dimensionnement de la FFT ===
    // Résolution fréquentielle = effectiveSampleRate / fftSize
    // Auto : ~0.5Hz de résolution → fftSize = effectiveSampleRate × 2
    // Arrondi à la puissance de 2 supérieure (requis pour FFTW optimal)
    if (m_userFftSize > 0) {
        m_fftSize = m_userFftSize;
    } else {
        m_fftSize = 1;
        size_t targetSize = static_cast<size_t>(m_effectiveSampleRate * 2);
        while (m_fftSize < targetSize) m_fftSize <<= 1;
    }

    // Nombre de samples bruts nécessaires pour remplir la FFT après décimation
    m_samplesNeeded = m_fftSize * m_decimFactor;

    // === Allocation des buffers FFTW ===
    // fftwf_alloc_* garantit l'alignement mémoire optimal (SIMD)
    if (m_fftPlan) fftwf_destroy_plan(m_fftPlan);
    if (m_fftIn)   fftwf_free(m_fftIn);
    if (m_fftOut)  fftwf_free(m_fftOut);

    m_fftIn  = fftwf_alloc_real(m_fftSize);                // N réels en entrée
    m_fftOut = fftwf_alloc_complex(m_fftSize / 2 + 1);     // N/2+1 complexes en sortie
    // FFTW_MEASURE : FFTW teste plusieurs algorithmes et garde le plus rapide.
    // C'est lent à la 1ère exécution mais ensuite la FFT est optimale.
    m_fftPlan = fftwf_plan_dft_r2c_1d(
        static_cast<int>(m_fftSize), m_fftIn, m_fftOut, FFTW_ESTIMATE
    );

    m_rawBuffer.clear();
    m_rawBuffer.reserve(m_samplesNeeded * 2); // Place pour 2 fenêtres

    // Init du pitch detector au sample rate décimé
    m_pitchDetector.init(m_effectiveSampleRate, 20.0f, m_cutoffHz + 20.0f);

    // Démarrage du chrono (une seule fois)
    if (!m_started) {
        m_startTime = std::chrono::steady_clock::now();
        m_started = true;
    }

    std::cout << "[BassDetector] Source: " << sourceSampleRate << " Hz\n";
    std::cout << "[BassDetector] Decimation: x" << m_decimFactor
              << " -> Effective: " << m_effectiveSampleRate << " Hz\n";
    std::cout << "[BassDetector] FFT size: " << m_fftSize
              << " (resolution: " << (float)m_effectiveSampleRate / m_fftSize << " Hz)\n";
    std::cout << "[BassDetector] Cutoff: " << m_cutoffHz << " Hz\n";
}

void BassDetector::feedSamples(const std::vector<float>& samples) {
    // Accumulation des samples dans le buffer brut
    m_rawBuffer.insert(m_rawBuffer.end(), samples.begin(), samples.end());

    // Traitement avec 50% d'overlap :
    // Dès qu'on a une fenêtre complète, on traite et on avance d'une demi-fenêtre.
    // L'overlap améliore le suivi temporel (2× plus d'analyses par seconde).
    size_t halfWindow = m_samplesNeeded / 2;
    while (m_rawBuffer.size() >= m_samplesNeeded) {
        processBuffer();
        m_rawBuffer.erase(m_rawBuffer.begin(), m_rawBuffer.begin() + halfWindow);
    }
}

void BassDetector::processBuffer() {
    // --- 1. Copie du chunk de données brutes ---
    std::vector<float> chunk(m_rawBuffer.begin(), m_rawBuffer.begin() + m_samplesNeeded);

    // --- 2. Filtrage passe-bas (au sample rate source, ex: 48kHz) ---
    // Le filtre Butterworth atténue tout au-dessus du cutoff.
    // IMPORTANT : filtrer AVANT de décimer, sinon aliasing !
    m_lowpass.process(chunk);

    // --- 3. Décimation ---
    // On garde 1 sample sur m_decimFactor → réduit massivement la taille
    auto decimated = DSP::decimate(chunk, m_decimFactor);
    decimated.resize(m_fftSize, 0.0f); // Pad avec des zéros si nécessaire

    // On garde une copie du signal décimé NON fenêtré pour l'autocorrélation.
    // La fenêtre de Hann fausserait la détection de périodicité.
    m_lastDecimated = decimated;

    // --- 4. Fenêtrage de Hann ---
    auto windowed = decimated;
    DSP::applyHannWindow(windowed);

    // --- 5. FFT ---
    std::copy(windowed.begin(), windowed.end(), m_fftIn);
    fftwf_execute(m_fftPlan);

    // --- 6. Analyse du spectre ---
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

    // Facteur de normalisation FFT (pour obtenir des dB absolus)
    float refMag = static_cast<float>(m_fftSize);

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

    // --- 7. Interpolation parabolique du pic FFT ---
    // On ajuste une parabole sur les 3 bins (gauche, centre, droite) autour du pic
    // pour estimer la vraie position du maximum avec précision sub-bin.
    //
    //   δ = 0.5 × (magL - magR) / (2·magC - magL - magR)
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
    float rmsDb  = 10.0f * log10f(std::max(totalEnergy / (refMag * refMag), 1e-20f));

    // Timestamp
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_startTime).count();

    // --- 8. Détection de pitch par autocorrélation ---
    // On utilise le signal décimé NON fenêtré (m_lastDecimated) car la fenêtre
    // de Hann atténue les bords et fausse la détection de périodicité.
    PitchInfo pitchResult = m_pitchDetector.detect(decimated);

    // --- 9. Assemblage du résultat ---
    BassInfo info;
    info.peakFrequency = peakFreq;
    info.peakMagnitude = peakDb;
    info.rmsLevel = rmsDb;
    info.valid = (peakMag > 0.001f * refMag); // Seuil de détection ≈ -60dB
    info.pitch = pitchResult;
    info.timestamp = elapsed;

    // Stockage thread-safe des résultats
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_latestResult = info;
        m_latestSpectrum = std::move(spectrum);
    }

    addHistoryEntry(info);
}

void BassDetector::addHistoryEntry(const BassInfo& info) {
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
    // Changer le cutoff nécessite de recalculer tout : décimation, filtre, FFT
    init(m_sourceSampleRate, hz, m_userFftSize);
}

void BassDetector::setFftSize(size_t size) {
    m_userFftSize = size;
    init(m_sourceSampleRate, m_cutoffHz, size);
}
