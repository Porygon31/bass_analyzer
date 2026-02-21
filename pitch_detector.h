/*
 * pitch_detector.h — Détection de pitch par autocorrélation (méthode McLeod NSDF)
 *
 * Pourquoi l'autocorrélation en plus de la FFT ?
 * -----------------------------------------------
 * La FFT a une résolution fréquentielle limitée (= sampleRate / fftSize).
 * Même avec interpolation parabolique, elle reste imprécise pour les basses
 * car un bin FFT à 400Hz/1024 ≈ 0.4Hz de résolution, c'est bien mais pas top.
 *
 * L'autocorrélation mesure la périodicité du signal directement dans le domaine
 * temporel. Pour un signal à 40Hz, la période est 25ms (= 10 samples à 400Hz).
 * L'autocorrélation trouve cette période avec une précision sub-échantillon
 * grâce à l'interpolation parabolique.
 *
 * Méthode McLeod (NSDF) :
 * Au lieu de l'autocorrélation brute r(τ), on utilise la Normalized Square
 * Difference Function qui normalise automatiquement et dont les pics sont
 * bornés entre -1 et 1. Ça évite le biais vers les petits lags et donne
 * directement une mesure de confiance.
 *
 * Ref: McLeod, P. "A smarter way to find pitch" (2005)
 */
#pragma once

#include <vector>
#include <cstdint>
#include <string>

struct PitchInfo {
    float frequency  = 0.0f;   // Fréquence détectée en Hz
    float confidence = 0.0f;   // Confiance de la détection [0.0, 1.0]
    bool  valid      = false;  // True si un pitch a été trouvé

    // Informations sur la note musicale
    std::string noteName;       // Nom de la note (ex: "E1", "A#0")
    float       cents = 0.0f;   // Déviation par rapport à la note la plus proche [-50, +50]
    int         midiNote = 0;   // Numéro MIDI (A4 = 69)
};

class PitchDetector {
public:
    PitchDetector() = default;

    // Initialise le détecteur pour un sample rate et une plage de fréquences donnés.
    // minFreqHz : fréquence minimum à détecter (défaut 20Hz, limite de l'audible)
    // maxFreqHz : fréquence maximum (défaut 120Hz, au-dessus de la plage basse)
    void init(uint32_t sampleRate, float minFreqHz = 20.0f, float maxFreqHz = 120.0f);

    // Détecte le pitch dans un buffer de samples mono float.
    // Retourne PitchInfo avec valid=false si aucun pitch trouvé.
    PitchInfo detect(const std::vector<float>& samples);

    // Utilitaire statique : convertit une fréquence en infos de note musicale
    static PitchInfo freqToNoteInfo(float freq);

private:
    uint32_t m_sampleRate = 0;
    float m_minFreq = 20.0f;
    float m_maxFreq = 120.0f;

    // Plage de lags (en échantillons) correspondant à la plage de fréquences
    // lag = sampleRate / freq → basse fréquence = grand lag
    size_t m_minLag = 0;  // = sampleRate / maxFreq
    size_t m_maxLag = 0;  // = sampleRate / minFreq

    // Buffer de la NSDF (réutilisé entre les appels pour éviter les allocations)
    std::vector<float> m_nsdf;
};
