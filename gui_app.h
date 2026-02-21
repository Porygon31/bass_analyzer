/*
 * gui_app.h — Application GUI ImGui + DirectX 11
 *
 * Architecture graphique :
 * - DirectX 11 pour le rendu (swapchain, render target)
 * - ImGui pour l'interface (widgets, dessin custom via ImDrawList)
 * - Pas de framework lourd : fenêtre Win32 native
 *
 * Panels de la GUI :
 * - Main Panel : fréquence en gros + note musicale + VU-mètre
 * - Controls Panel : sliders cutoff/smoothing, checkboxes
 * - Spectrum Panel : visualisation spectrale (barres + courbe)
 * - History Panel : graphe temporel des fréquences (~30s)
 * - Pitch Panel : tuner chromatique avec indicateur de cents
 *
 * Smoothing du spectre :
 * - Basé sur le delta-time (indépendant du framerate)
 * - Asymétrique : attack rapide (montée) / release plus lent (descente)
 *   → comme un compresseur audio, le spectre "attrape" les pics vite
 *     mais redescend de façon contrôlée. Beaucoup plus lisible.
 * - Le slider "Speed" contrôle la vitesse globale (1=brut, 10=lent)
 */
#pragma once

#include "audio_capture.h"
#include "bass_detector.h"
#include <d3d11.h>
#include <string>
#include <chrono>
#include <mutex>

class GuiApp {
public:
    GuiApp();
    ~GuiApp();

    // Initialise la fenêtre Win32, DirectX 11, ImGui, et l'audio WASAPI
    bool init(int width = 1280, int height = 800);

    // Boucle principale (bloquante). Retourne quand la fenêtre est fermée.
    void run();

private:
    // --- DirectX 11 ---
    bool createDeviceD3D(HWND hWnd);
    void cleanupDeviceD3D();
    void createRenderTarget();
    void cleanupRenderTarget();

    // --- Rendu ImGui ---
    void renderFrame();
    void drawMainPanel();      // Affichage principal (freq, note, VU)
    void drawSpectrumPanel();  // Spectre FFT
    void drawHistoryPanel();   // Historique temporel
    void drawPitchPanel();     // Tuner chromatique
    void drawControlsPanel();  // Sliders et options
    void drawScopePanel();     // Oscilloscope

    // --- CPU usage ---
    void updateCpuUsage();

    // --- Audio callback ---
    AudioCallback makeAudioCallback();

    // --- Helpers de dessin custom (via ImDrawList) ---
    void drawSpectrumPlot(const SpectrumData& spectrum, float width, float height);
    void drawHistoryPlot(const std::vector<HistoryEntry>& history, float width, float height);
    void drawPitchMeter(const PitchInfo& pitch, float width, float height);
    void drawScopePlot(float width, float height);

    // --- Fenêtre Win32 ---
    HWND        m_hwnd = nullptr;
    WNDCLASSEXW m_wc   = {};
    int m_width  = 1280;
    int m_height = 800;

    // --- DirectX 11 ---
    ID3D11Device*           m_d3dDevice = nullptr;
    ID3D11DeviceContext*    m_d3dContext = nullptr;
    IDXGISwapChain*         m_swapChain = nullptr;
    ID3D11RenderTargetView* m_rtv       = nullptr;
    bool m_swapChainOccluded = false;

    // --- Audio ---
    AudioCapture m_capture;
    BassDetector m_detector;
    float        m_cutoff       = 100.0f; // Fréquence de coupure actuelle
    bool         m_audioRunning = false;

    // --- Sélection du périphérique audio ---
    std::vector<AudioDeviceInfo> m_deviceList;
    int  m_selectedDeviceIndex  = 0;    // Index dans m_deviceList
    bool m_followDefaultDevice  = true; // Auto-switch quand le défaut Windows change

    // --- FFT ---
    int m_fftSizeIndex = 0; // 0 = Auto, 1 = 512, 2 = 1024, ...

    // --- État de l'interface ---
    bool  m_showDemo     = false; // Afficher la démo ImGui (debug)
    bool  m_showSpectrum = true;
    bool  m_showHistory  = true;
    bool  m_showPitch    = true;
    bool  m_showScope    = true;

    // --- Smoothing du spectre ---
    // smoothSpeed contrôle la réactivité globale du spectre lissé.
    //   1.0  = brut (pas de lissage, réponse immédiate)
    //   5.0  = smooth moyen (bon compromis lisibilité/réactivité)
    //   10.0 = très lissé (molasses, joli mais lent)
    //
    // L'attack est ~3× plus rapide que le release pour que les pics
    // soient captés immédiatement mais descendent de façon smooth.
    float m_smoothSpeed = 5.0f;
    std::vector<float> m_smoothedSpectrum;

    // Delta-time pour un smoothing indépendant du framerate
    std::chrono::steady_clock::time_point m_lastFrameTime;
    float m_deltaTime = 0.016f; // ~60fps par défaut

    // --- Oscilloscope (ring buffer ~400ms @ 48kHz) ---
    static constexpr size_t SCOPE_BUFFER_SIZE = 19200;
    std::vector<float> m_scopeBuffer;
    size_t             m_scopeHead = 0;
    std::mutex         m_scopeMutex;

    // --- CPU usage du processus ---
    float m_cpuUsage       = 0.0f;  // % CPU affiché
    float m_cpuUpdateTimer = 0.0f;  // Accumulateur pour échantillonnage ~500ms
    ULARGE_INTEGER m_prevKernel{};   // Temps kernel précédent
    ULARGE_INTEGER m_prevUser{};     // Temps user précédent
    ULARGE_INTEGER m_prevTime{};     // Temps réel précédent
    int m_numCores = 1;              // Nombre de cœurs logiques
};

// Gestionnaire de messages Win32 (forward déclaré pour ImGui)
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
