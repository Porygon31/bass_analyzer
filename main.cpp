/*
 * main.cpp — Point d'entrée de l'application Bass Analyzer
 *
 * Deux modes de compilation :
 *
 * 1. GUI (par défaut) — target: bass_analyzer
 *    Utilise WinMain (pas de console), lance la fenêtre ImGui/DX11.
 *
 * 2. Console (debug) — target: bass_analyzer_console
 *    Compilé avec BASS_CONSOLE_MODE=1, affiche les données en texte
 *    dans le terminal avec des couleurs ANSI. Utile pour débugger
 *    sans la surcouche graphique.
 */
#include "gui_app.h"
#include "audio_capture.h"
#include "bass_detector.h"
#include <iostream>
#include <memory>
#include <string>
#include <cstring>

#ifdef BASS_CONSOLE_MODE
// ============================================================
// MODE CONSOLE (pour debug / serveur headless)
// ============================================================
#include <iomanip>
#include <thread>
#include <chrono>
#include <conio.h>  // _kbhit(), _getch() — spécifique Windows
#include <cmath>

void runConsoleMode() {
    // Active les séquences d'échappement ANSI dans le terminal Windows
    // (nécessaire pour les couleurs depuis Windows 10 1511)
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hConsole, &mode);
    SetConsoleMode(hConsole, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    std::cout << "\033[96m\033[1m"
              << "Bass Analyzer - Console Mode\n"
              << "\033[0m\n";

    // Init audio
    AudioCapture capture;
    if (!capture.init()) {
        std::cerr << "\033[91m[ERROR] Failed to initialize audio capture!\033[0m\n";
        return;
    }

    // Init détecteur
    BassDetector detector;
    float cutoff = 100.0f;
    detector.init(capture.getSampleRate(), cutoff);

    // Démarre la capture (callback depuis le thread audio)
    capture.start([&detector](const std::vector<float>& samples, uint32_t sr) {
        detector.feedSamples(samples);
    });

    std::cout << "\033[90m[Controls] Q=Quit | +/- = Cutoff | R=Reset\n\033[0m\n";

    // Boucle d'affichage ~20fps
    bool running = true;
    while (running) {
        // Gestion clavier non-bloquante
        if (_kbhit()) {
            char ch = _getch();
            switch (ch) {
                case 'q': case 'Q': case 27: running = false; break; // ESC ou Q
                case '+': case '=':
                    cutoff = std::min<float>(cutoff + 10.0f, 500.0f);
                    detector.setCutoff(cutoff);
                    break;
                case '-': case '_':
                    cutoff = std::max<float>(cutoff - 10.0f, 30.0f);
                    detector.setCutoff(cutoff);
                    break;
                case 'r': case 'R':
                    cutoff = 100.0f;
                    detector.setCutoff(cutoff);
                    break;
            }
        }

        auto result = detector.getLatestResult();

        // Efface la ligne et réécrit (affichage in-place)
        std::cout << "\r\033[K"; // \r = retour chariot, \033[K = efface jusqu'à la fin
        if (result.valid) {
            // Fréquence FFT en cyan
            std::cout << " \033[1m\033[97mBASS: \033[96m"
                      << std::fixed << std::setprecision(1) << std::setw(6)
                      << result.peakFrequency << " Hz\033[0m";

            // Note musicale en jaune (si pitch détecté)
            if (result.pitch.valid) {
                std::cout << " \033[93m" << result.pitch.noteName
                          << " " << std::showpos << std::fixed << std::setprecision(0)
                          << result.pitch.cents << "c\033[0m"
                          << std::noshowpos;
            }

            // dB et cutoff en gris
            std::cout << " \033[90m[" << std::fixed << std::setprecision(1)
                      << result.peakMagnitude << " dB]"
                      << "  Cut: " << static_cast<int>(cutoff) << "Hz\033[0m";
        } else {
            std::cout << " \033[90mBASS: --- no signal ---  Cut: "
                      << static_cast<int>(cutoff) << "Hz\033[0m";
        }
        std::cout << std::flush;

        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // ~20fps
    }

    capture.stop();
    std::cout << "\n\n\033[92mStopped.\033[0m\n";
}

int main(int argc, char* argv[]) {
    runConsoleMode();
    return 0;
}

#else
// ============================================================
// MODE GUI (par défaut)
// ============================================================
// WinMain est le point d'entrée pour les applications Windows GUI
// (pas de fenêtre console créée)
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
                   _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    auto app = std::make_unique<GuiApp>();

    if (!app->init(1400, 900)) {
        return 1;
    }

    app->run(); // Boucle principale (bloquante)
    return 0;
}
#endif
