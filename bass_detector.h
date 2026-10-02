/*
 * bass_detector.h — Détecteur de basses : FFT + autocorrélation + historique
 *
 * C'est le cœur du pipeline d'analyse. Il orchestre :
 * 1. Le filtrage passe-bas + décimation en flux (à l'arrivée de chaque paquet)
 * 2. Une fenêtre glissante d'échantillons décimés (300ms par défaut)
 * 3. L'analyse FFT (spectre + pic fréquentiel), recalculée toutes les ~10ms
 * 4. La détection de pitch par autocorrélation
 * 5. Le stockage du spectre pour la GUI
 * 6. L'historique temporel (ring buffer)
 *
 * Latence : la durée de la fenêtre fixe le compromis réactivité / précision.
 * Le résultat reflète le son d'environ (fenêtre / 2) en arrière.
 *
 * Thread-safety : le traitement et la reconfiguration (cutoff, fenêtre,
 * sample rate) sont protégés par un mutex, car le feed vient du thread audio
 * et les réglages du thread GUI. Les résultats ont leurs propres mutex.
 */
#pragma once

#include "dsp_utils.h"
#include "pitch_detector.h"
#include <fftw3.h>
#include <vector>
#include <array>
#include <cstdint>
#include <mutex>
#include <chrono>

// Résultat complet d'une analyse bass
struct BassInfo {
    float peakFrequency   = 0.0f;  // Fréquence du pic FFT (Hz)
    float stableFrequency = 0.0f;  // Médiane des derniers pics (Hz) — affichage sans scintillement
    float peakMagnitude   = 0.0f;  // Magnitude du pic (dB)
    float rmsLevel        = 0.0f;  // Niveau RMS de la bande bass (dB)
    bool  valid           = false; // True si un signal significatif est détecté
    PitchInfo pitch;               // Résultat de l'autocorrélation
    double timestamp = 0.0;        // Secondes depuis le démarrage
};

// Snapshot du spectre FFT pour affichage GUI
struct SpectrumData {
    std::vector<float> magnitudesDb; // Magnitude de chaque bin en dB
    std::vector<float> frequencies;   // Fréquence de chaque bin en Hz
    float freqPerBin = 0.0f;         // Pas de la grille fréquentielle (Hz/bin)
    size_t numBins = 0;               // Nombre total de bins
    float cutoffHz = 100.0f;          // Fréquence de coupure actuelle
};

// Entrée de l'historique temporel
struct HistoryEntry {
    double timestamp       = 0.0;
    float  peakFrequency   = 0.0f;
    float  peakMagnitude   = 0.0f;
    float  pitchFrequency  = 0.0f;
    float  pitchConfidence = 0.0f;
    std::string noteName;
    bool   valid = false;
};

class BassDetector {
public:
    // Taille du ring buffer d'historique
    // 600 entrées × 50ms entre chaque = ~30 secondes de données
    static constexpr size_t HISTORY_SIZE = 600;

    BassDetector();
    ~BassDetector();

    // Initialise le détecteur pour un sample rate source et un cutoff donné.
    // Calcule le facteur de décimation, dimensionne la fenêtre et la FFT, etc.
    // La durée de fenêtre courante (setWindowMs) est conservée.
    void init(uint32_t sourceSampleRate, float bassCutoffHz = 100.0f);

    // Alimente le détecteur en nouveaux échantillons mono float.
    // Appelé depuis le thread audio (WASAPI). Déclenche une analyse
    // dès qu'un nouveau hop (~10ms) d'échantillons est disponible.
    void feedSamples(const std::vector<float>& samples);

    // --- Getters thread-safe (appelés depuis le thread GUI) ---
    BassInfo                getLatestResult();
    SpectrumData            getSpectrum();
    std::vector<HistoryEntry> getHistory();

    // Change le cutoff en temps réel (re-initialise le pipeline)
    void setCutoff(float hz);

    // Change la durée de la fenêtre d'analyse en ms (re-initialise le pipeline).
    // Plus courte = plus réactif, plus longue = notes proches mieux séparées.
    void setWindowMs(float ms);

    // Accesseurs de configuration (modifiée uniquement depuis le thread GUI)
    float    getCutoff()              const { return m_cutoffHz; }
    float    getWindowMs()            const { return m_windowMs; }
    uint32_t getEffectiveSampleRate() const { return m_effectiveSampleRate; }
    size_t   getFFTSize()             const { return m_fftSize; }
    size_t   getWindowLen()           const { return m_windowLen; }
    size_t   getHopLen()              const { return m_hopLen; }
    int      getDecimationFactor()    const { return m_decimFactor; }

private:
    // Recalcule tout le pipeline : décimation, filtre, fenêtre, FFT.
    // Doit être appelé avec m_processMutex verrouillé.
    void configure();

    // Analyse la fenêtre la plus récente : FFT → pitch → résultat
    void processBuffer();

    // Ajoute une entrée dans le ring buffer d'historique
    void addHistoryEntry(const BassInfo& info);

    // --- Paramètres du pipeline ---
    uint32_t m_sourceSampleRate    = 48000; // Sample rate du device WASAPI
    float    m_cutoffHz            = 100.0f; // Fréquence de coupure bass
    float    m_windowMs            = 300.0f; // Durée de la fenêtre d'analyse (ms)
    int      m_decimFactor         = 1;      // Facteur de décimation (ex: 120)
    uint32_t m_effectiveSampleRate = 48000;  // Sample rate après décimation (ex: 400Hz)
    size_t   m_windowLen           = 0;      // Fenêtre FFT en samples décimés (ex: 120)
    size_t   m_fftSize             = 0;      // Taille FFT (puissance de 2 ≥ m_windowLen, zero-padding)
    size_t   m_nsdfLen             = 0;      // Fenêtre de l'autocorrélation en samples décimés
    size_t   m_hopLen              = 1;      // Samples décimés entre deux analyses (~10ms)

    // --- Modules DSP ---
    DSP::LowPassFilter m_lowpass;       // Filtre Butterworth anti-aliasing
    PitchDetector      m_pitchDetector; // Détecteur de pitch par autocorrélation

    // --- Traitement en flux (protégé par m_processMutex) ---
    std::vector<float> m_filtered;      // Buffer de travail : paquet en cours de filtrage
    std::vector<float> m_decBuffer;     // Fenêtre glissante des derniers samples décimés
    int    m_decimPhase = 0;            // Position dans le cycle de décimation
    size_t m_newSamples = 0;            // Samples décimés reçus depuis la dernière analyse
    std::mutex m_processMutex;

    // --- FFTW ---
    float*         m_fftIn   = nullptr; // Buffer d'entrée FFT (réel)
    fftwf_complex* m_fftOut  = nullptr; // Buffer de sortie FFT (complexe)
    fftwf_plan     m_fftPlan = nullptr; // Plan FFTW

    // --- Derniers pics, pour la médiane anti-scintillement ---
    static constexpr size_t STABLE_COUNT = 5; // 5 analyses ≈ 50ms
    std::array<float, STABLE_COUNT> m_recentPeaks{};
    size_t m_recentHead  = 0;
    size_t m_recentCount = 0;

    // --- Résultats (protégés par mutex) ---
    BassInfo     m_latestResult;
    SpectrumData m_latestSpectrum;
    std::mutex   m_resultMutex;

    // --- Historique (ring buffer protégé par mutex) ---
    std::array<HistoryEntry, HISTORY_SIZE> m_history;
    size_t     m_historyHead  = 0; // Index d'écriture (tête du ring buffer)
    size_t     m_historyCount = 0; // Nombre d'entrées valides
    double     m_lastHistoryTs = -1.0; // Timestamp de la dernière entrée (1 entrée / 50ms)
    std::mutex m_historyMutex;

    // --- Timing ---
    std::chrono::steady_clock::time_point m_startTime;
    bool m_started = false;
};
