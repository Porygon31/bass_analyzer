/*
 * bass_detector.h — Détecteur de basses : FFT + autocorrélation + historique
 *
 * C'est le cœur du pipeline d'analyse. Il orchestre :
 * 1. L'accumulation des échantillons bruts
 * 2. Le filtrage passe-bas + décimation
 * 3. L'analyse FFT (spectre + pic fréquentiel)
 * 4. La détection de pitch par autocorrélation
 * 5. Le stockage du spectre pour la GUI
 * 6. L'historique temporel (ring buffer)
 *
 * Thread-safety : les résultats sont protégés par mutex car le feed
 * vient du thread audio et la lecture vient du thread GUI.
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
    float peakFrequency = 0.0f;  // Fréquence du pic FFT (Hz)
    float peakMagnitude = 0.0f;  // Magnitude du pic (dB)
    float rmsLevel      = 0.0f;  // Niveau RMS de la bande bass (dB)
    bool  valid         = false; // True si un signal significatif est détecté
    PitchInfo pitch;              // Résultat de l'autocorrélation
    double timestamp = 0.0;       // Secondes depuis le démarrage
};

// Snapshot du spectre FFT pour affichage GUI
struct SpectrumData {
    std::vector<float> magnitudesDb; // Magnitude de chaque bin en dB
    std::vector<float> frequencies;   // Fréquence de chaque bin en Hz
    float freqPerBin = 0.0f;         // Résolution fréquentielle (Hz/bin)
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
    // 600 entrées × ~50ms entre chaque = ~30 secondes de données
    static constexpr size_t HISTORY_SIZE = 600;

    BassDetector();
    ~BassDetector();

    // Initialise le détecteur pour un sample rate source et un cutoff donné.
    // Calcule le facteur de décimation, dimensionne la FFT, etc.
    // fftSize = 0 → auto (puissance de 2 >= effectiveSampleRate × 2)
    void init(uint32_t sourceSampleRate, float bassCutoffHz = 100.0f, size_t fftSize = 0);

    // Alimente le détecteur en nouveaux échantillons mono float.
    // Appelé depuis le thread audio (WASAPI). Déclenche automatiquement
    // une analyse quand assez d'échantillons sont accumulés.
    void feedSamples(const std::vector<float>& samples);

    // --- Getters thread-safe (appelés depuis le thread GUI) ---
    BassInfo                getLatestResult();
    SpectrumData            getSpectrum();
    std::vector<HistoryEntry> getHistory();

    // Change le cutoff en temps réel (re-initialise le pipeline)
    void setCutoff(float hz);

    // Change la taille FFT (0 = auto, sinon puissance de 2)
    void setFftSize(size_t size);

    // Accesseurs
    float    getCutoff()            const { return m_cutoffHz; }
    uint32_t getEffectiveSampleRate() const { return m_effectiveSampleRate; }
    size_t   getFFTSize()           const { return m_fftSize; }
    int      getDecimationFactor()  const { return m_decimFactor; }

private:
    // Traite un buffer complet : filtre → décime → FFT → pitch → résultat
    void processBuffer();

    // Ajoute une entrée dans le ring buffer d'historique
    void addHistoryEntry(const BassInfo& info);

    // --- Paramètres du pipeline ---
    uint32_t m_sourceSampleRate    = 48000; // Sample rate du device WASAPI
    float    m_cutoffHz            = 100.0f; // Fréquence de coupure bass
    int      m_decimFactor         = 1;      // Facteur de décimation (ex: 120)
    uint32_t m_effectiveSampleRate = 48000;  // Sample rate après décimation (ex: 400Hz)
    size_t   m_fftSize             = 0;      // Taille FFT (puissance de 2)
    size_t   m_userFftSize         = 0;      // 0 = auto, sinon taille forcée par l'utilisateur

    // --- Modules DSP ---
    DSP::LowPassFilter m_lowpass;       // Filtre Butterworth anti-aliasing
    PitchDetector      m_pitchDetector; // Détecteur de pitch par autocorrélation

    // --- Buffer d'accumulation ---
    std::vector<float> m_rawBuffer;     // Échantillons bruts en attente de traitement
    size_t m_samplesNeeded = 0;         // Nombre de samples bruts pour une analyse complète

    // --- FFTW ---
    float*         m_fftIn   = nullptr; // Buffer d'entrée FFT (réel)
    fftwf_complex* m_fftOut  = nullptr; // Buffer de sortie FFT (complexe)
    fftwf_plan     m_fftPlan = nullptr; // Plan FFTW (pré-optimisé)

    // Dernier buffer décimé (gardé pour l'autocorrélation)
    std::vector<float> m_lastDecimated;

    // --- Résultats (protégés par mutex) ---
    BassInfo     m_latestResult;
    SpectrumData m_latestSpectrum;
    std::mutex   m_resultMutex;

    // --- Historique (ring buffer protégé par mutex) ---
    std::array<HistoryEntry, HISTORY_SIZE> m_history;
    size_t     m_historyHead  = 0; // Index d'écriture (tête du ring buffer)
    size_t     m_historyCount = 0; // Nombre d'entrées valides
    std::mutex m_historyMutex;

    // --- Timing ---
    std::chrono::steady_clock::time_point m_startTime;
    bool m_started = false;
};
