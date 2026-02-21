/*
 * dsp_utils.h — Utilitaires de traitement numérique du signal (DSP)
 *
 * Contient les briques de base du pipeline audio :
 * - Filtre passe-bas Butterworth 4ème ordre (IIR, 2 biquads en cascade)
 * - Décimation (sous-échantillonnage par facteur entier)
 * - Fenêtrage de Hann (pour la FFT)
 *
 * Le filtre Butterworth est choisi car il a la réponse la plus plate
 * possible dans la bande passante (pas de ripple), ce qui est idéal
 * pour un filtre anti-aliasing avant décimation.
 */
#pragma once

#include <vector>
#include <cstdint>

namespace DSP {

    /*
     * Filtre passe-bas IIR Butterworth 4ème ordre
     *
     * Implémenté comme 2 sections biquad (Second-Order Sections) en cascade.
     * Chaque biquad est un filtre du 2ème ordre, 2 en cascade = 4ème ordre.
     *
     * Avantages du Butterworth :
     * - Réponse plate dans la bande passante (pas de ripple)
     * - Roll-off de -24 dB/octave (4ème ordre)
     * - Bonne atténuation au-delà du cutoff
     *
     * La conception utilise la transformée bilinéaire avec pre-warping
     * pour mapper le prototype analogique vers le domaine numérique.
     */
    class LowPassFilter {
    public:
        LowPassFilter() = default;

        // Calcule les coefficients du filtre pour une fréquence de coupure donnée.
        // cutoffHz : fréquence de coupure en Hz
        // sampleRate : fréquence d'échantillonnage du signal à filtrer
        void design(float cutoffHz, float sampleRate);

        // Filtre les échantillons in-place (modifie le vecteur directement).
        // Conserve l'état interne entre les appels (z1, z2) pour un filtrage continu.
        void process(std::vector<float>& samples);

        // Remet à zéro l'état interne du filtre (supprime les transitoires)
        void reset();

    private:
        // Structure d'une section biquad (filtre du 2ème ordre)
        // Forme directe transposée II (Direct Form II Transposed)
        //
        // Equation : y[n] = b0*x[n] + z1
        //            z1   = b1*x[n] - a1*y[n] + z2
        //            z2   = b2*x[n] - a2*y[n]
        struct Biquad {
            float b0 = 1, b1 = 0, b2 = 0; // Coefficients numérateur (zéros)
            float a1 = 0, a2 = 0;          // Coefficients dénominateur (pôles)
            float z1 = 0, z2 = 0;          // État interne (mémoire du filtre)
        };

        Biquad m_sections[2]; // 2 biquads = 4ème ordre Butterworth
        bool m_designed = false;
    };

    // Décimation : réduit le sample rate par un facteur entier.
    // Garde 1 échantillon sur 'factor'. Le signal DOIT être filtré passe-bas
    // AVANT la décimation pour éviter l'aliasing (repliement de spectre).
    //
    // Ex: décimation x120 → 48000 Hz → 400 Hz
    // Ça réduit massivement la quantité de données à traiter par la FFT.
    std::vector<float> decimate(const std::vector<float>& input, int factor);

    // Applique une fenêtre de Hann in-place.
    // La fenêtre de Hann atténue les bords du signal vers zéro, ce qui réduit
    // le "spectral leakage" (fuite spectrale) lors de la FFT.
    // Sans fenêtrage, la FFT "voit" des discontinuités aux bords du buffer
    // et génère des artefacts haute fréquence parasites.
    //
    // Formule : w[n] = 0.5 * (1 - cos(2π*n / (N-1)))
    void applyHannWindow(std::vector<float>& samples);

} // namespace DSP
