/*
 * gui_app.cpp — Implémentation de l'application GUI
 *
 * Le rendu utilise ImGui en mode "full-window" : une seule fenêtre ImGui
 * qui occupe tout l'espace client, avec les différents panels empilés.
 *
 * Le dessin custom (spectre, historique, tuner) utilise ImDrawList
 * pour dessiner directement des primitives (lignes, rectangles, triangles).
 *
 * Smoothing du spectre :
 * - Basé sur le delta-time (indépendant du framerate)
 * - Asymétrique : attack rapide (montée) / release lent (descente)
 *   → Le spectre "attrape" les pics immédiatement mais redescend smooth.
 *   → Similaire au comportement d'un compresseur audio (attack/release).
 */
#include "gui_app.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>
#include <algorithm>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Conversion FILETIME → ULARGE_INTEGER (pour calculs CPU usage)
static ULARGE_INTEGER toULI(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli;
}

// Forward declare du handler Win32 d'ImGui
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// === Palette de couleurs (IM_COL32 = RGBA) ===
#define COL_BG       IM_COL32(0x1A, 0x1A, 0x2E, 0xFF)
#define COL_PANEL    IM_COL32(0x16, 0x21, 0x3E, 0xFF)
#define COL_ACCENT   IM_COL32(0x0F, 0x34, 0x60, 0xFF)
#define COL_CYAN     IM_COL32(0x00, 0xFF, 0xD1, 0xFF) // Couleur principale
#define COL_YELLOW   IM_COL32(0xFF, 0xD9, 0x3D, 0xFF) // Pitch autocorr
#define COL_GREEN    IM_COL32(0x6B, 0xCB, 0x77, 0xFF) // Niveaux faibles
#define COL_RED      IM_COL32(0xFF, 0x6B, 0x6B, 0xFF) // Niveaux forts
#define COL_ORANGE   IM_COL32(0xFF, 0xA0, 0x40, 0xFF)
#define COL_MAGENTA  IM_COL32(0xE9, 0x45, 0x60, 0xFF) // Marqueur peak
#define COL_DIM      IM_COL32(0x80, 0x80, 0x80, 0xFF) // Texte secondaire
#define COL_GRID     IM_COL32(0x30, 0x30, 0x50, 0x80) // Grille

// === Gestionnaire de messages Win32 ===
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:      return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:   PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

GuiApp::GuiApp() {}

GuiApp::~GuiApp() {
    m_capture.stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(m_hwnd);
    UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
}

bool GuiApp::init(int width, int height) {
    m_width = width;
    m_height = height;

    // --- Fenêtre Win32 ---
    m_wc = {sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L,
            GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
            L"BassAnalyzerClass", nullptr};
    RegisterClassExW(&m_wc);
    m_hwnd = CreateWindowW(m_wc.lpszClassName, L"Bass Analyzer v0.2",
                           WS_OVERLAPPEDWINDOW, 100, 100, m_width, m_height,
                           nullptr, nullptr, m_wc.hInstance, nullptr);

    if (!createDeviceD3D(m_hwnd)) {
        cleanupDeviceD3D();
        UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
        return false;
    }
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);

    // --- Setup ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Style custom sombre
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding  = 4.0f;
    style.GrabRounding   = 4.0f;
    style.WindowPadding  = ImVec2(10, 10);
    style.FramePadding   = ImVec2(8, 4);
    style.ItemSpacing    = ImVec2(8, 6);

    // Couleurs du thème
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]         = ImVec4(0.10f, 0.10f, 0.18f, 1.00f);
    colors[ImGuiCol_ChildBg]          = ImVec4(0.09f, 0.13f, 0.24f, 1.00f);
    colors[ImGuiCol_Border]           = ImVec4(0.06f, 0.20f, 0.38f, 0.80f);
    colors[ImGuiCol_FrameBg]          = ImVec4(0.06f, 0.13f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.10f, 0.20f, 0.35f, 1.00f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.12f, 0.25f, 0.40f, 1.00f);
    colors[ImGuiCol_TitleBg]          = ImVec4(0.06f, 0.10f, 0.20f, 1.00f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(0.06f, 0.15f, 0.30f, 1.00f);
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.00f, 1.00f, 0.82f, 0.80f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.00f, 1.00f, 0.82f, 1.00f);
    colors[ImGuiCol_Button]           = ImVec4(0.06f, 0.20f, 0.38f, 1.00f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.10f, 0.30f, 0.50f, 1.00f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.91f, 0.27f, 0.38f, 1.00f);
    colors[ImGuiCol_Header]           = ImVec4(0.06f, 0.20f, 0.38f, 0.80f);
    colors[ImGuiCol_HeaderHovered]    = ImVec4(0.10f, 0.30f, 0.50f, 1.00f);
    colors[ImGuiCol_HeaderActive]     = ImVec4(0.91f, 0.27f, 0.38f, 1.00f);
    colors[ImGuiCol_PlotLines]        = ImVec4(0.00f, 1.00f, 0.82f, 1.00f);
    colors[ImGuiCol_PlotHistogram]    = ImVec4(0.91f, 0.27f, 0.38f, 1.00f);
    colors[ImGuiCol_CheckMark]        = ImVec4(0.00f, 1.00f, 0.82f, 1.00f);

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_d3dDevice, m_d3dContext);

    // --- Audio WASAPI ---
    if (!m_capture.init()) {
        MessageBoxA(m_hwnd, "Failed to initialize WASAPI loopback.\nCheck audio output.",
                    "Audio Error", MB_OK | MB_ICONERROR);
        return false;
    }
    m_detector.init(m_capture.getSampleRate(), m_cutoff);
    m_scopeBuffer.resize(SCOPE_BUFFER_SIZE, 0.0f);
    m_capture.start(makeAudioCallback());
    m_audioRunning = true;

    // Énumération initiale des devices audio
    m_deviceList = m_capture.enumerateDevices();
    updateDeviceIndex();

    // Init du chrono pour le delta-time
    m_lastFrameTime = std::chrono::steady_clock::now();

    // Init du suivi CPU
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    m_numCores = si.dwNumberOfProcessors;
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    m_prevKernel = toULI(kernel);
    m_prevUser   = toULI(user);
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    m_prevTime = toULI(nowFt);

    return true;
}

void GuiApp::run() {
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        if (m_swapChainOccluded && m_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        m_swapChainOccluded = false;
        renderFrame();
    }
}

void GuiApp::renderFrame() {
    // --- Delta-time indépendant du framerate ---
    auto now = std::chrono::steady_clock::now();
    m_deltaTime = std::chrono::duration<float>(now - m_lastFrameTime).count();
    m_deltaTime = std::clamp(m_deltaTime, 0.001f, 0.1f);
    m_lastFrameTime = now;

    // --- Polling des changements de périphériques audio ---
    if (m_capture.hasDeviceListChanged()) {
        m_deviceList = m_capture.enumerateDevices();
        updateDeviceIndex();
    }
    if (m_capture.hasDefaultDeviceChanged() && m_followDefaultDevice) {
        uint32_t oldSr = m_capture.getSampleRate();
        if (m_capture.switchDevice(L"", makeAudioCallback())) {
            if (m_capture.getSampleRate() != oldSr)
                m_detector.init(m_capture.getSampleRate(), m_cutoff);
            m_deviceList = m_capture.enumerateDevices();
            updateDeviceIndex();
        }
    }

    // --- Mise à jour CPU usage (~toutes les 500ms) ---
    m_cpuUpdateTimer += m_deltaTime;
    if (m_cpuUpdateTimer >= 0.5f) {
        updateCpuUsage();
        m_cpuUpdateTimer = 0.0f;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Full-window layout
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##Main", nullptr, flags);

    drawMainPanel();
    drawControlsPanel();
    ImGui::Separator();
    if (m_showSpectrum) drawSpectrumPanel();
    if (m_showHistory)  drawHistoryPanel();
    if (m_showPitch)    drawPitchPanel();
    if (m_showScope)    drawScopePanel();
    if (m_showDemo) ImGui::ShowDemoWindow(&m_showDemo);

    ImGui::End();

    // Rendu DX11
    ImGui::Render();
    const float clear[4] = {0.10f, 0.10f, 0.18f, 1.00f};
    m_d3dContext->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_d3dContext->ClearRenderTargetView(m_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    HRESULT hr = m_swapChain->Present(1, 0);
    m_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
}

// =============================================================================
// MAIN PANEL
// =============================================================================

void GuiApp::drawMainPanel() {
    auto result = m_detector.getLatestResult();

    // Titre + device name
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.82f, 1.0f));
    ImGui::Text("BASS ANALYZER");
    ImGui::PopStyleColor();
    // Espace disponible à droite du titre (relatif à la fenêtre, tient compte scrollbar)
    float titleW = ImGui::GetItemRectSize().x;
    float contentMaxX = ImGui::GetContentRegionMax().x;
    float availW = contentMaxX - titleW - 16;

    // Formate le texte CPU + device
    std::string deviceName = m_capture.getDeviceName();
    std::string suffix = m_followDefaultDevice ? " (auto)" : "";
    char infoBuf[256];
    snprintf(infoBuf, sizeof(infoBuf), "CPU: %.1f%%  |  Device: %s%s",
        m_cpuUsage, deviceName.c_str(), suffix.c_str());

    // Si le texte est trop large, tronquer le nom du device avec "..."
    float infoW = ImGui::CalcTextSize(infoBuf).x;
    if (infoW > availW && deviceName.size() > 10) {
        while (infoW > availW && deviceName.size() > 10) {
            deviceName.pop_back();
            snprintf(infoBuf, sizeof(infoBuf), "CPU: %.1f%%  |  Device: %s...%s",
                m_cpuUsage, deviceName.c_str(), suffix.c_str());
            infoW = ImGui::CalcTextSize(infoBuf).x;
        }
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(contentMaxX - infoW);
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", infoBuf);
    ImGui::Spacing();

    if (result.valid) {
        // Fréquence en gros (font ×2.5)
        ImGui::PushFont(nullptr);
        char freqBuf[64];
        snprintf(freqBuf, sizeof(freqBuf), "%.1f Hz", result.peakFrequency);
        float oldScale = ImGui::GetFont()->Scale;
        ImGui::GetFont()->Scale = 2.5f;
        ImGui::PushFont(ImGui::GetFont());
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.82f, 1.0f), "%s", freqBuf);
        ImGui::GetFont()->Scale = oldScale;
        ImGui::PopFont();
        ImGui::PopFont();

        // Note musicale à côté
        ImGui::SameLine();
        if (result.pitch.valid) {
            ImGui::BeginGroup();
            ImGui::Spacing(); ImGui::Spacing();
            float c = result.pitch.confidence;
            ImGui::TextColored(ImVec4(c, 1.0f, 0.82f * c, 1.0f), "%s", result.pitch.noteName.c_str());
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%.1f Hz (autocorr)", result.pitch.frequency);
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%+.0f cents | conf: %.0f%%",
                              result.pitch.cents, result.pitch.confidence * 100.0f);
            ImGui::EndGroup();
        }

        // VU-mètre dB (barre horizontale avec gradient)
        ImGui::Spacing();
        float fraction = std::clamp((result.peakMagnitude + 60.0f) / 60.0f, 0.0f, 1.0f);
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        float barW = ImGui::GetContentRegionAvail().x, barH = 20.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(barPos, ImVec2(barPos.x + barW, barPos.y + barH),
                         IM_COL32(0x10, 0x10, 0x20, 0xFF), 4.0f);
        float fillW = barW * fraction;
        if (fillW > 2.0f) {
            ImU32 colR = (fraction > 0.8f) ? COL_RED : (fraction > 0.6f ? COL_YELLOW : COL_CYAN);
            dl->AddRectFilledMultiColor(barPos, ImVec2(barPos.x + fillW, barPos.y + barH),
                                       COL_GREEN, colR, colR, COL_GREEN);
        }
        char dbBuf[32];
        snprintf(dbBuf, sizeof(dbBuf), "%.1f dB", result.peakMagnitude);
        ImVec2 ts = ImGui::CalcTextSize(dbBuf);
        ImVec2 textPos(barPos.x + barW/2 - ts.x/2, barPos.y + barH/2 - ts.y/2);
        bool barCoversText = (fillW > barW / 2);
        ImU32 outlineCol = barCoversText ? IM_COL32(0xFF, 0xFF, 0xFF, 0xBB) : IM_COL32(0x00, 0x00, 0x00, 0xBB);
        ImU32 textCol    = barCoversText ? IM_COL32(0x00, 0x00, 0x00, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
        dl->AddText(ImVec2(textPos.x - 1, textPos.y), outlineCol, dbBuf);
        dl->AddText(ImVec2(textPos.x + 1, textPos.y), outlineCol, dbBuf);
        dl->AddText(ImVec2(textPos.x, textPos.y - 1), outlineCol, dbBuf);
        dl->AddText(ImVec2(textPos.x, textPos.y + 1), outlineCol, dbBuf);
        dl->AddText(textPos, textCol, dbBuf);
        ImGui::Dummy(ImVec2(0, barH + 4));
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No bass signal detected...");
        ImGui::Spacing();
    }
}

// =============================================================================
// CONTROLS PANEL
// =============================================================================

void GuiApp::drawControlsPanel() {
    ImGui::Spacing();

    // --- Sélection du périphérique audio ---
    if (!m_deviceList.empty()) {
        ImGui::SetNextItemWidth(350);
        const char* preview = (m_selectedDeviceIndex < (int)m_deviceList.size())
            ? m_deviceList[m_selectedDeviceIndex].name.c_str()
            : "Unknown";

        if (ImGui::BeginCombo("Audio Device", preview)) {
            for (int i = 0; i < (int)m_deviceList.size(); i++) {
                bool isSelected = (i == m_selectedDeviceIndex);
                std::string label = m_deviceList[i].name;
                if (m_deviceList[i].isDefault) label += " (Default)";

                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    if (i != m_selectedDeviceIndex) {
                        m_selectedDeviceIndex = i;
                        m_followDefaultDevice = m_deviceList[i].isDefault;

                        uint32_t oldSr = m_capture.getSampleRate();
                        std::wstring targetId = m_followDefaultDevice
                            ? L"" : m_deviceList[i].id;
                        if (m_capture.switchDevice(targetId, makeAudioCallback())) {
                            if (m_capture.getSampleRate() != oldSr)
                                m_detector.init(m_capture.getSampleRate(), m_cutoff);
                        }
                    }
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Follow Default", &m_followDefaultDevice);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto-switch when Windows default audio device changes");
    }

    // Ligne 2 : Cutoff + Reset + FFT Size + Speed
    ImGui::SetNextItemWidth(250);
    if (ImGui::SliderFloat("Bass Cutoff (Hz)", &m_cutoff, 30.0f, 500.0f, "%.0f Hz")) {
        m_capture.stop();
        m_detector.setCutoff(m_cutoff);
        m_capture.start(makeAudioCallback());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##cutoff")) {
        m_cutoff = 100.0f;
        m_capture.stop();
        m_detector.setCutoff(m_cutoff);
        m_capture.start(makeAudioCallback());
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    static const struct { const char* label; size_t value; } fftOptions[] = {
        {"Auto", 0}, {"512", 512}, {"1024", 1024}, {"2048", 2048},
        {"4096", 4096}, {"8192", 8192}, {"16384", 16384}
    };
    if (ImGui::BeginCombo("FFT Size", fftOptions[m_fftSizeIndex].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(fftOptions); i++) {
            if (ImGui::Selectable(fftOptions[i].label, i == m_fftSizeIndex)) {
                m_fftSizeIndex = i;
                m_capture.stop();
                m_detector.setFftSize(fftOptions[i].value);
                m_capture.start(makeAudioCallback());
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        float res = (float)m_detector.getEffectiveSampleRate() / (float)m_detector.getFFTSize();
        ImGui::SetTooltip("FFT: %zu pts | Resolution: %.2f Hz", m_detector.getFFTSize(), res);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Speed", &m_smoothSpeed, 1.0f, 10.0f, "%.1f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smoothing speed\n1 = raw (instant)\n5 = balanced\n10 = very smooth");

    // Ligne 3 : Checkboxes panels
    ImGui::Checkbox("Spectrum", &m_showSpectrum);
    ImGui::SameLine();
    ImGui::Checkbox("History", &m_showHistory);
    ImGui::SameLine();
    ImGui::Checkbox("Pitch", &m_showPitch);
    ImGui::SameLine();
    ImGui::Checkbox("Scope", &m_showScope);
    ImGui::Spacing();
}

// =============================================================================
// SPECTRUM PANEL
// =============================================================================

void GuiApp::drawSpectrumPanel() {
    if (!ImGui::CollapsingHeader("Spectrum Analyzer", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto spectrum = m_detector.getSpectrum();
    if (spectrum.numBins == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Waiting for data...");
        return;
    }
    drawSpectrumPlot(spectrum, ImGui::GetContentRegionAvail().x, 200.0f);
}

void GuiApp::drawSpectrumPlot(const SpectrumData& spectrum, float width, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                     IM_COL32(0x08, 0x08, 0x14, 0xFF), 4.0f);

    // === Smoothing asymétrique basé sur le delta-time ===
    //
    // Chaque bin du spectre est lissé indépendamment avec :
    // - Attack (montée vers un pic) : RAPIDE, ×3 la vitesse du release
    //   → Les transitoires et impacts de basse sont captés immédiatement
    // - Release (descente après un pic) : LENT, contrôlé
    //   → Le spectre redescend de façon fluide et lisible
    //
    // Formule indépendante du framerate :
    //   alpha = 1 - exp(-deltaTime * rate)
    //   smoothed += (target - smoothed) * alpha
    //
    // À 60fps (dt=0.016s) et 144fps (dt=0.007s), le résultat visuel
    // est le même grâce à l'exponentielle.
    //
    // Le slider "Speed" contrôle les taux :
    //   speed=1  → attackRate=30, releaseRate=10 (quasi instantané)
    //   speed=5  → attackRate=6,  releaseRate=2  (bon compromis)
    //   speed=10 → attackRate=3,  releaseRate=1  (très smooth)

    if (m_smoothedSpectrum.size() != spectrum.magnitudesDb.size()) {
        m_smoothedSpectrum = spectrum.magnitudesDb;
    } else {
        float attackRate  = 30.0f / m_smoothSpeed; // Montée rapide
        float releaseRate = 10.0f / m_smoothSpeed; // Descente contrôlée

        for (size_t i = 0; i < spectrum.magnitudesDb.size(); i++) {
            float target  = spectrum.magnitudesDb[i];
            float current = m_smoothedSpectrum[i];
            // Attack si target > current (pic montant), release sinon
            float rate  = (target > current) ? attackRate : releaseRate;
            float alpha = 1.0f - expf(-m_deltaTime * rate);
            m_smoothedSpectrum[i] = current + (target - current) * alpha;
        }
    }

    float dbMin = -80.0f, dbMax = 0.0f;

    // Grille dB (horizontale)
    for (float db = -60.0f; db <= 0.0f; db += 20.0f) {
        float y = pos.y + height - (db - dbMin) / (dbMax - dbMin) * height;
        dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + width, y), COL_GRID);
        char label[16]; snprintf(label, sizeof(label), "%+.0f dB", db);
        dl->AddText(ImVec2(pos.x + 4, y - 12), COL_DIM, label);
    }

    // Grille Hz (verticale, tous les 10Hz, label tous les 20Hz)
    for (float freq = 20.0f; freq <= spectrum.cutoffHz; freq += 10.0f) {
        float x = pos.x + (freq / spectrum.cutoffHz) * width;
        dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + height), COL_GRID);
        if (fmodf(freq, 20.0f) < 0.1f) {
            char label[16]; snprintf(label, sizeof(label), "%.0f", freq);
            dl->AddText(ImVec2(x - 8, pos.y + height - 14), COL_DIM, label);
        }
    }

    // Barres spectrales
    size_t numBins = m_smoothedSpectrum.size();
    if (numBins < 2) return;
    float barW = width / static_cast<float>(numBins);

    for (size_t i = 1; i < numBins; i++) {
        float normalized = std::clamp((m_smoothedSpectrum[i] - dbMin) / (dbMax - dbMin), 0.0f, 1.0f);
        float barH = normalized * height;
        float x = pos.x + i * barW;
        float y = pos.y + height - barH;

        ImU32 col;
        if (normalized > 0.8f)      col = COL_RED;
        else if (normalized > 0.5f) col = COL_YELLOW;
        else if (normalized > 0.2f) col = COL_CYAN;
        else                         col = IM_COL32(0x00, 0x80, 0x60, 0xA0);

        if (barW > 3.0f) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + barW - 1, pos.y + height), col, 1.0f);
        else              dl->AddLine(ImVec2(x, y), ImVec2(x, pos.y + height), col, 2.0f);
    }

    // Courbe spectrale (polyline)
    std::vector<ImVec2> points;
    for (size_t i = 1; i < numBins; i++) {
        float normalized = std::clamp((m_smoothedSpectrum[i] - dbMin) / (dbMax - dbMin), 0.0f, 1.0f);
        points.push_back(ImVec2(pos.x + i * barW + barW * 0.5f, pos.y + height - normalized * height));
    }
    if (points.size() > 1)
        dl->AddPolyline(points.data(), (int)points.size(), COL_CYAN, ImDrawFlags_None, 2.0f);

    // Marqueur du pic FFT (ligne magenta)
    auto result = m_detector.getLatestResult();
    if (result.valid && result.peakFrequency > 0) {
        float peakX = pos.x + (result.peakFrequency / spectrum.cutoffHz) * width;
        dl->AddLine(ImVec2(peakX, pos.y), ImVec2(peakX, pos.y + height), COL_MAGENTA, 2.0f);
        char peakLabel[32]; snprintf(peakLabel, sizeof(peakLabel), "%.1f Hz", result.peakFrequency);
        dl->AddText(ImVec2(peakX + 4, pos.y + 4), COL_MAGENTA, peakLabel);
    }

    ImGui::Dummy(ImVec2(0, height + 4));
}

// =============================================================================
// HISTORY PANEL
// =============================================================================

void GuiApp::drawHistoryPanel() {
    if (!ImGui::CollapsingHeader("Frequency History", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto history = m_detector.getHistory();
    if (history.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Accumulating history...");
        return;
    }
    drawHistoryPlot(history, ImGui::GetContentRegionAvail().x, 160.0f);
}

void GuiApp::drawHistoryPlot(const std::vector<HistoryEntry>& history, float width, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                     IM_COL32(0x08, 0x08, 0x14, 0xFF), 4.0f);

    if (history.size() < 2) { ImGui::Dummy(ImVec2(0, height + 4)); return; }

    float freqMin = 15.0f, freqMax = m_cutoff + 10.0f;

    // Grille Hz
    for (float freq = 20.0f; freq <= freqMax; freq += 10.0f) {
        float y = pos.y + height - (freq - freqMin) / (freqMax - freqMin) * height;
        dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + width, y), COL_GRID);
        if (fmodf(freq, 20.0f) < 0.1f) {
            char label[16]; snprintf(label, sizeof(label), "%.0f Hz", freq);
            dl->AddText(ImVec2(pos.x + 4, y - 12), COL_DIM, label);
        }
    }

    // Deux courbes : FFT peak (cyan) + pitch autocorr (jaune)
    size_t N = history.size();
    float xStep = width / static_cast<float>(N);
    std::vector<ImVec2> fftPoints, pitchPoints;

    for (size_t i = 0; i < N; i++) {
        float x = pos.x + i * xStep;
        if (history[i].valid) {
            float fy = pos.y + height - std::clamp((history[i].peakFrequency - freqMin) / (freqMax - freqMin), 0.0f, 1.0f) * height;
            fftPoints.push_back(ImVec2(x, fy));
            if (history[i].pitchConfidence > 0.5f) {
                float py = pos.y + height - std::clamp((history[i].pitchFrequency - freqMin) / (freqMax - freqMin), 0.0f, 1.0f) * height;
                pitchPoints.push_back(ImVec2(x, py));
            }
        }
    }

    if (fftPoints.size() > 1)
        dl->AddPolyline(fftPoints.data(), (int)fftPoints.size(), COL_CYAN, ImDrawFlags_None, 2.0f);
    for (auto& pt : pitchPoints)
        dl->AddCircleFilled(pt, 3.0f, COL_YELLOW);

    // Légende
    dl->AddText(ImVec2(pos.x + width - 200, pos.y + 4), COL_CYAN, "--- FFT Peak");
    dl->AddCircleFilled(ImVec2(pos.x + width - 200 + 4, pos.y + 22), 3.0f, COL_YELLOW);
    dl->AddText(ImVec2(pos.x + width - 188, pos.y + 16), COL_YELLOW, "Autocorr Pitch");

    ImGui::Dummy(ImVec2(0, height + 4));
}

// =============================================================================
// PITCH PANEL — Tuner chromatique
// =============================================================================

void GuiApp::drawPitchPanel() {
    if (!ImGui::CollapsingHeader("Pitch Detection", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto result = m_detector.getLatestResult();
    if (!result.pitch.valid) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No pitch detected...");
        return;
    }
    drawPitchMeter(result.pitch, ImGui::GetContentRegionAvail().x, 80.0f);
}

void GuiApp::drawPitchMeter(const PitchInfo& pitch, float width, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                     IM_COL32(0x08, 0x08, 0x14, 0xFF), 4.0f);
    float centerY = pos.y + height * 0.5f;

    // Nom de la note en gros
    {
        float oldScale = ImGui::GetFont()->Scale;
        ImGui::GetFont()->Scale = 2.0f;
        ImGui::PushFont(ImGui::GetFont());
        dl->AddText(ImVec2(pos.x + 20, centerY - ImGui::CalcTextSize(pitch.noteName.c_str()).y / 2),
                   COL_CYAN, pitch.noteName.c_str());
        ImGui::GetFont()->Scale = oldScale;
        ImGui::PopFont();
    }

    // Mètre de cents [-50, +50]
    float meterX = pos.x + 120, meterW = width - 160, meterH = 30.0f;
    float meterY = centerY - meterH / 2;

    dl->AddRectFilled(ImVec2(meterX, meterY), ImVec2(meterX + meterW, meterY + meterH),
                     IM_COL32(0x10, 0x10, 0x20, 0xFF), 4.0f);
    // Centre (note juste)
    dl->AddLine(ImVec2(meterX + meterW/2, meterY), ImVec2(meterX + meterW/2, meterY + meterH),
               IM_COL32(0xFF, 0xFF, 0xFF, 0x40), 1.0f);

    // Graduations (tous les 10 cents)
    for (int c = -50; c <= 50; c += 10) {
        float x = meterX + (c + 50.0f) / 100.0f * meterW;
        float tickH = (c == 0) ? meterH : meterH * 0.3f;
        dl->AddLine(ImVec2(x, meterY + (meterH - tickH)/2), ImVec2(x, meterY + (meterH + tickH)/2),
                   IM_COL32(0x60, 0x60, 0x60, 0xFF));
    }

    // Indicateur (triangle + barre)
    // Vert = juste (<±5c), jaune = proche (<±15c), rouge = faux
    float indicatorX = meterX + (std::clamp(pitch.cents / 50.0f, -1.0f, 1.0f) + 1.0f) * 0.5f * meterW;
    ImU32 iCol = (std::abs(pitch.cents) < 5.0f) ? COL_GREEN :
                 (std::abs(pitch.cents) < 15.0f) ? COL_YELLOW : COL_RED;
    dl->AddTriangleFilled(ImVec2(indicatorX, meterY - 2),
                         ImVec2(indicatorX - 6, meterY - 10),
                         ImVec2(indicatorX + 6, meterY - 10), iCol);
    dl->AddRectFilled(ImVec2(indicatorX - 2, meterY), ImVec2(indicatorX + 2, meterY + meterH), iCol, 1.0f);

    // Labels cents
    char centsBuf[32]; snprintf(centsBuf, sizeof(centsBuf), "%+.0f cents", pitch.cents);
    dl->AddText(ImVec2(meterX + meterW/2 - 25, meterY + meterH + 2), COL_DIM, centsBuf);
    dl->AddText(ImVec2(meterX, meterY + meterH + 2), COL_DIM, "-50c");
    dl->AddText(ImVec2(meterX + meterW - ImGui::CalcTextSize("+50c").x, meterY + meterH + 2), COL_DIM, "+50c");

    // Barre de confiance
    float confY = meterY + meterH + 18;
    dl->AddRectFilled(ImVec2(meterX, confY), ImVec2(meterX + meterW, confY + 6),
                     IM_COL32(0x10, 0x10, 0x20, 0xFF), 2.0f);
    dl->AddRectFilled(ImVec2(meterX, confY), ImVec2(meterX + meterW * pitch.confidence, confY + 6),
                     COL_GREEN, 2.0f);
    char confBuf[32]; snprintf(confBuf, sizeof(confBuf), "Confidence: %.0f%%", pitch.confidence * 100.0f);
    dl->AddText(ImVec2(meterX + meterW + 8, confY - 4), COL_DIM, confBuf);

    ImGui::Dummy(ImVec2(0, height + 10));
}

// =============================================================================
// HELPERS
// =============================================================================

void GuiApp::updateDeviceIndex() {
    auto currentId = m_capture.getCurrentDeviceId();
    m_selectedDeviceIndex = 0;
    for (int i = 0; i < (int)m_deviceList.size(); i++) {
        if (m_deviceList[i].id == currentId) {
            m_selectedDeviceIndex = i;
            break;
        }
    }
}

// =============================================================================
// AUDIO CALLBACK
// =============================================================================

AudioCallback GuiApp::makeAudioCallback() {
    return [this](const std::vector<float>& samples, uint32_t sr) {
        m_detector.feedSamples(samples);
        // Alimenter le ring buffer oscilloscope
        std::lock_guard<std::mutex> lock(m_scopeMutex);
        for (float s : samples) {
            m_scopeBuffer[m_scopeHead] = s;
            m_scopeHead = (m_scopeHead + 1) % SCOPE_BUFFER_SIZE;
        }
    };
}

// =============================================================================
// OSCILLOSCOPE
// =============================================================================

void GuiApp::drawScopePanel() {
    if (!ImGui::CollapsingHeader("Oscilloscope", ImGuiTreeNodeFlags_DefaultOpen)) return;
    drawScopePlot(ImGui::GetContentRegionAvail().x, 160.0f);
}

void GuiApp::drawScopePlot(float width, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                     IM_COL32(0x08, 0x08, 0x14, 0xFF), 4.0f);

    // Copier le ring buffer sous mutex
    std::vector<float> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_scopeMutex);
        if (m_scopeBuffer.empty()) {
            ImGui::Dummy(ImVec2(0, height + 4));
            return;
        }
        snapshot.resize(SCOPE_BUFFER_SIZE);
        for (size_t i = 0; i < SCOPE_BUFFER_SIZE; i++) {
            snapshot[i] = m_scopeBuffer[(m_scopeHead + i) % SCOPE_BUFFER_SIZE];
        }
    }

    // Ligne centrale (zéro)
    float centerY = pos.y + height * 0.5f;
    dl->AddLine(ImVec2(pos.x, centerY), ImVec2(pos.x + width, centerY), COL_GRID);

    // Graduations d'amplitude (±0.5, ±1.0)
    for (float amp : {0.25f, 0.5f, 0.75f, 1.0f}) {
        float yUp   = centerY - amp * (height * 0.45f);
        float yDown = centerY + amp * (height * 0.45f);
        dl->AddLine(ImVec2(pos.x, yUp), ImVec2(pos.x + width, yUp), COL_GRID);
        dl->AddLine(ImVec2(pos.x, yDown), ImVec2(pos.x + width, yDown), COL_GRID);
    }

    // Sous-échantillonner pour le dessin (1 point par pixel max)
    size_t numPoints = (std::min)(snapshot.size(), static_cast<size_t>(width));
    float step = static_cast<float>(snapshot.size()) / static_cast<float>(numPoints);

    std::vector<ImVec2> points;
    points.reserve(numPoints);
    for (size_t i = 0; i < numPoints; i++) {
        size_t idx = static_cast<size_t>(i * step);
        float x = pos.x + static_cast<float>(i) / static_cast<float>(numPoints) * width;
        float y = centerY - snapshot[idx] * (height * 0.45f);
        points.push_back(ImVec2(x, y));
    }

    if (points.size() > 1)
        dl->AddPolyline(points.data(), static_cast<int>(points.size()), COL_GREEN, ImDrawFlags_None, 1.5f);

    // Label durée
    dl->AddText(ImVec2(pos.x + 4, pos.y + 4), COL_DIM, "~400ms");

    ImGui::Dummy(ImVec2(0, height + 4));
}

// =============================================================================
// CPU USAGE
// =============================================================================

void GuiApp::updateCpuUsage() {
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);

    ULARGE_INTEGER curKernel = toULI(kernel);
    ULARGE_INTEGER curUser   = toULI(user);
    ULARGE_INTEGER curTime   = toULI(nowFt);

    uint64_t cpuDelta = (curKernel.QuadPart - m_prevKernel.QuadPart)
                      + (curUser.QuadPart - m_prevUser.QuadPart);
    uint64_t timeDelta = curTime.QuadPart - m_prevTime.QuadPart;

    if (timeDelta > 0) {
        m_cpuUsage = 100.0f * static_cast<float>(cpuDelta)
                   / static_cast<float>(timeDelta * m_numCores);
    }

    m_prevKernel = curKernel;
    m_prevUser = curUser;
    m_prevTime = curTime;
}

// =============================================================================
// DIRECTX 11
// =============================================================================

bool GuiApp::createDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount                 = 2;
    sd.BufferDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate       = {60, 1};
    sd.Flags                       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                 = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                = hWnd;
    sd.SampleDesc                  = {1, 0};
    sd.Windowed                    = TRUE;
    sd.SwapEffect                  = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &m_swapChain, &m_d3dDevice, &featureLevel, &m_d3dContext);
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}

void GuiApp::cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice)  { m_d3dDevice->Release();  m_d3dDevice = nullptr; }
}

void GuiApp::createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    m_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
    backBuffer->Release();
}

void GuiApp::cleanupRenderTarget() {
    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
}