/*
 * dsp_utils.cpp — Implémentation des utilitaires DSP
 *
 * Conception du filtre Butterworth :
 * 1. On part du prototype analogique (pôles sur le cercle unité en s)
 * 2. On applique le pre-warping pour corriger la distorsion fréquentielle
 *    de la transformée bilinéaire (tan(wc/2))
 * 3. On transforme chaque paire de pôles conjugués en section biquad
 *    via la transformée bilinéaire s → z
 *
 * Pour un Butterworth d'ordre N, les pôles sont espacés uniformément
 * sur le demi-cercle gauche du plan s, aux angles : π(2k+1)/(2N)
 */
#include "dsp_utils.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace DSP {

void LowPassFilter::design(float cutoffHz, float sampleRate) {
    // === Étape 1 : Pre-warping ===
    // La transformée bilinéaire compresse les fréquences (warping).
    // On pré-distord la fréquence de coupure pour que le filtre numérique
    // ait sa coupure exactement à cutoffHz.
    float wc = 2.0f * static_cast<float>(M_PI) * cutoffHz / sampleRate;
    float wc_warped = 2.0f * sampleRate * tanf(wc / 2.0f);

    // === Étape 2 : Angles des pôles du Butterworth 4ème ordre ===
    // Formule : θk = π(2k+1)/(2N) pour k = 0..N-1, N = 4
    // On groupe les pôles conjugués en paires → 2 sections biquad
    // Section 0 : pôles à 5π/8 et 3π/8 (conjugués)
    // Section 1 : pôles à 7π/8 et π/8   (conjugués)
    float angles[2] = {
        static_cast<float>(M_PI) * 5.0f / 8.0f,
        static_cast<float>(M_PI) * 7.0f / 8.0f
    };

    // === Étape 3 : Transformée bilinéaire pour chaque section ===
    for (int i = 0; i < 2; i++) {
        float re = cosf(angles[i]); // Partie réelle du pôle analogique
        float im = sinf(angles[i]); // Partie imaginaire (non utilisée directement)

        // Prototype analogique passe-bas :
        //   H(s) = wc² / (s² - 2·re·wc·s + wc²)
        //
        // Substitution bilinéaire : s = 2·fs·(z-1)/(z+1)
        // Après développement, on obtient les coefficients biquad numériques.
        float K   = 2.0f * sampleRate;
        float K2  = K * K;
        float wc2 = wc_warped * wc_warped;

        // Coefficient de normalisation (dénominateur commun)
        float a0 = K2 - 2.0f * re * wc_warped * K + wc2;

        // Coefficients numérateur (numérateur = wc² × (1 + 2z⁻¹ + z⁻²))
        m_sections[i].b0 = wc2 / a0;
        m_sections[i].b1 = 2.0f * wc2 / a0;
        m_sections[i].b2 = wc2 / a0;

        // Coefficients dénominateur
        m_sections[i].a1 = 2.0f * (wc2 - K2) / a0;
        m_sections[i].a2 = (K2 + 2.0f * re * wc_warped * K + wc2) / a0;

        // Reset de l'état
        m_sections[i].z1 = 0;
        m_sections[i].z2 = 0;
    }

    m_designed = true;
}

void LowPassFilter::process(std::vector<float>& samples) {
    if (!m_designed) return;

    // On applique les 2 biquads en cascade (section 0 puis section 1).
    // Forme directe transposée II — numériquement stable et efficace.
    //
    //   y[n] = b0·x[n] + z1
    //   z1   = b1·x[n] - a1·y[n] + z2
    //   z2   = b2·x[n] - a2·y[n]
    //
    // L'état (z1, z2) persiste entre les appels, ce qui permet de filtrer
    // un flux audio continu par morceaux.
    for (auto& section : m_sections) {
        for (auto& x : samples) {
            float y = section.b0 * x + section.z1;
            section.z1 = section.b1 * x - section.a1 * y + section.z2;
            section.z2 = section.b2 * x - section.a2 * y;
            x = y; // Le sample filtré remplace l'original
        }
    }
}

void LowPassFilter::reset() {
    for (auto& s : m_sections) {
        s.z1 = 0;
        s.z2 = 0;
    }
}

std::vector<float> decimate(const std::vector<float>& input, int factor) {
    // Décimation = garder 1 échantillon sur N.
    // Simple mais efficace. Le filtre passe-bas en amont garantit
    // que les fréquences au-dessus de Nyquist/2 du nouveau sample rate
    // sont déjà atténuées → pas d'aliasing.
    if (factor <= 1) return input;

    std::vector<float> output;
    output.reserve(input.size() / factor + 1);

    for (size_t i = 0; i < input.size(); i += factor) {
        output.push_back(input[i]);
    }

    return output;
}

void applyHannWindow(std::vector<float>& samples) {
    // Fenêtre de Hann : w[n] = 0.5 · (1 - cos(2πn / (N-1)))
    //
    // Propriétés :
    // - Atténue les bords du signal vers zéro (taper)
    // - Réduit le spectral leakage de la FFT
    // - Bon compromis résolution fréquentielle / dynamic range
    // - Largeur du lobe principal : 4 bins FFT
    // - Atténuation du premier lobe secondaire : -31 dB
    size_t N = samples.size();
    if (N == 0) return;

    for (size_t i = 0; i < N; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * static_cast<float>(M_PI) * i / (N - 1)));
        samples[i] *= w;
    }
}

} // namespace DSP
