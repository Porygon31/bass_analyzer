/*
 * pitch_detector.cpp — Implémentation de la détection de pitch McLeod NSDF
 *
 * Pipeline :
 * 1. Calcul de la NSDF pour chaque lag dans [minLag, maxLag]
 * 2. Recherche du premier pic significatif (> threshold)
 *    → On préfère le premier pic car c'est la fondamentale
 *    (les pics suivants sont les harmoniques/sous-multiples)
 * 3. Interpolation parabolique autour du pic pour précision sub-échantillon
 * 4. Conversion lag → fréquence → note musicale
 */
#include "pitch_detector.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void PitchDetector::init(uint32_t sampleRate, float minFreqHz, float maxFreqHz) {
    m_sampleRate = sampleRate;
    m_minFreq = minFreqHz;
    m_maxFreq = maxFreqHz;

    // Relation inverse entre fréquence et lag (période en échantillons)
    // freq = sampleRate / lag  →  lag = sampleRate / freq
    //
    // Haute fréquence (120Hz) → petit lag (ex: 400/120 ≈ 3 samples)
    // Basse fréquence (20Hz)  → grand lag (ex: 400/20  = 20 samples)
    m_minLag = static_cast<size_t>(sampleRate / maxFreqHz);
    m_maxLag = static_cast<size_t>(sampleRate / minFreqHz);
}

PitchInfo PitchDetector::detect(const std::vector<float>& samples) {
    PitchInfo result;
    size_t N = samples.size();

    // Il faut au moins 2x le lag max d'échantillons pour que
    // l'autocorrélation ait assez de données
    if (N < m_maxLag * 2 || m_sampleRate == 0) return result;

    // === Calcul de la Normalized Square Difference Function (NSDF) ===
    //
    // Formule :
    //   NSDF(τ) = 2·r(τ) / m(τ)
    //
    // Où :
    //   r(τ) = Σ x[i]·x[i+τ]          (autocorrélation classique)
    //   m(τ) = Σ (x[i]² + x[i+τ]²)    (énergie combinée)
    //
    // La normalisation par m(τ) borne les valeurs entre -1 et 1.
    // Un pic à 1.0 = signal parfaitement périodique à cette période.
    // Un pic à 0.5 = signal assez périodique (seuil typique de détection).

    m_nsdf.resize(m_maxLag + 1);

    for (size_t tau = m_minLag; tau <= m_maxLag; tau++) {
        float acf    = 0.0f; // Autocorrélation r(τ)
        float energy = 0.0f; // Terme de normalisation m(τ)

        size_t len = N - tau;
        for (size_t i = 0; i < len; i++) {
            acf    += samples[i] * samples[i + tau];
            energy += samples[i] * samples[i] + samples[i + tau] * samples[i + tau];
        }

        // Protection contre la division par zéro (silence)
        if (energy > 1e-10f) {
            m_nsdf[tau] = 2.0f * acf / energy;
        } else {
            m_nsdf[tau] = 0.0f;
        }
    }

    // === Recherche de pics ("Key Maximums" de McLeod) ===
    //
    // Stratégie :
    // 1. Attendre que la NSDF passe en négatif (première demi-période)
    // 2. Chercher le premier pic local au-dessus du seuil
    // 3. C'est la fondamentale (les pics suivants = harmoniques)
    //
    // Le seuil est mis à 0.5 (plus bas que le 0.7 classique) car les basses
    // pures ont parfois des harmoniques faibles qui réduisent la NSDF.

    const float threshold = 0.5f;
    float  bestPeak = -1.0f;
    size_t bestLag  = 0;
    bool foundNegative  = false;
    bool foundFirstPeak = false;

    for (size_t tau = m_minLag + 1; tau < m_maxLag; tau++) {
        // On attend le premier passage en négatif
        if (m_nsdf[tau] < 0.0f) {
            foundNegative = true;
        }

        // Détection de pic local : NSDF[tau] > NSDF[tau-1] ET NSDF[tau] >= NSDF[tau+1]
        if (foundNegative && m_nsdf[tau] > m_nsdf[tau - 1] && m_nsdf[tau] >= m_nsdf[tau + 1]) {
            if (m_nsdf[tau] > threshold) {
                if (!foundFirstPeak) {
                    // Premier pic trouvé → c'est la fondamentale
                    bestPeak = m_nsdf[tau];
                    bestLag  = tau;
                    foundFirstPeak = true;
                } else if (m_nsdf[tau] > bestPeak) {
                    // Pic ultérieur plus fort → le prendre seulement s'il est
                    // significativement meilleur (peut arriver avec des signaux complexes)
                    bestPeak = m_nsdf[tau];
                    bestLag  = tau;
                }

                // Si on a trouvé un pic très fort (>0.7), on s'arrête
                // → c'est assez confiant pour être la fondamentale
                if (foundFirstPeak && m_nsdf[tau] > 0.7f) break;
            }
        }
    }

    // Rien trouvé au-dessus du seuil → pas de pitch détecté
    if (bestLag == 0 || bestPeak < threshold) return result;

    // === Interpolation parabolique pour précision sub-échantillon ===
    //
    // On ajuste une parabole sur les 3 points autour du pic (a, b, c)
    // et on trouve le sommet analytique :
    //   δ = (a - c) / (2·(2b - a - c))
    //   lag_interpolé = bestLag + δ
    //
    // Ça donne une précision bien meilleure que la résolution en samples.
    // Ex: à 400Hz sample rate, 1 sample = 2.5ms → sans interpolation on a
    // une résolution de ±1Hz à 40Hz. Avec interpolation : ~0.1Hz.
    float lagInterp = static_cast<float>(bestLag);
    if (bestLag > m_minLag && bestLag < m_maxLag) {
        float a = m_nsdf[bestLag - 1];
        float b = m_nsdf[bestLag];
        float c = m_nsdf[bestLag + 1];
        float denom = 2.0f * (2.0f * b - a - c);
        if (std::abs(denom) > 1e-10f) {
            float delta = (a - c) / denom;
            lagInterp = static_cast<float>(bestLag) + delta;

            // Valeur interpolée du pic (pour la confiance)
            bestPeak = b - 0.25f * (a - c) * delta;
        }
    }

    // Conversion lag → fréquence
    float freq = static_cast<float>(m_sampleRate) / lagInterp;

    // Remplissage du résultat avec les infos de note
    result = freqToNoteInfo(freq);
    result.confidence = std::max(0.0f, std::min(1.0f, bestPeak));
    result.valid = true;

    return result;
}

PitchInfo PitchDetector::freqToNoteInfo(float freq) {
    PitchInfo info;
    info.frequency = freq;

    if (freq < 8.0f) return info; // En dessous de C-1, pas de note utile

    // Table des noms de notes chromatiques
    const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    // Conversion fréquence → numéro MIDI
    // Formule : MIDI = 12·log2(freq/440) + 69
    // A4 (440Hz) = MIDI 69, chaque demi-ton = +1
    float midiFloat = 12.0f * log2f(freq / 440.0f) + 69.0f;
    int midiNote = static_cast<int>(roundf(midiFloat));

    info.midiNote = midiNote;

    // Cents = déviation par rapport à la note la plus proche
    // 100 cents = 1 demi-ton, range [-50, +50]
    info.cents = (midiFloat - static_cast<float>(midiNote)) * 100.0f;

    // Extraction du nom de note et de l'octave
    int noteIndex = midiNote % 12;
    if (noteIndex < 0) noteIndex += 12;
    int octave = (midiNote / 12) - 1;

    info.noteName = std::string(noteNames[noteIndex]) + std::to_string(octave);

    return info;
}
