/*
 * audio_capture.h — Capture audio en loopback via WASAPI
 *
 * WASAPI Loopback permet de capturer le son qui sort des enceintes/casque
 * sans passer par un micro. On récupère directement le flux audio interne
 * du PC, ce qui est parfait pour analyser la musique en cours de lecture.
 *
 * Le flux est converti en mono float [-1.0, 1.0] avant d'être envoyé
 * au callback, quel que soit le format natif du device (16bit, 24bit, 32bit float).
 */
#pragma once

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <string>

// Callback appelé à chaque paquet audio capturé.
// Reçoit les échantillons mono float et le sample rate du device.
using AudioCallback = std::function<void(const std::vector<float>&, uint32_t sampleRate)>;

// Informations sur un périphérique audio
struct AudioDeviceInfo {
    std::wstring id;        // ID WASAPI (ex: "{0.0.0.00000000}.{guid}")
    std::string  name;      // Nom lisible UTF-8 (ex: "Speakers (Realtek HD Audio)")
    bool         isDefault; // True si c'est le device par défaut eRender/eConsole
};

// Forward declare
class AudioCapture;

// Client de notification pour détecter les changements de périphériques audio.
// Implémente IMMNotificationClient (interface COM callback de Windows).
class DeviceNotificationClient : public IMMNotificationClient {
public:
    DeviceNotificationClient(AudioCapture* owner);

    // IUnknown
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

private:
    LONG m_refCount = 1;
    AudioCapture* m_owner;
};

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Initialise WASAPI en mode loopback.
    // deviceId vide = device par défaut, sinon utilise le device spécifié.
    bool init(const std::wstring& deviceId = L"");

    // Démarre la capture dans un thread dédié.
    // Le callback est appelé depuis ce thread (~toutes les 10ms).
    bool start(AudioCallback callback);

    // Arrête la capture et join le thread.
    void stop();

    // Énumère tous les périphériques de sortie audio actifs.
    std::vector<AudioDeviceInfo> enumerateDevices();

    // Change de périphérique : stop → release → reinit → start.
    // deviceId vide = device par défaut.
    bool switchDevice(const std::wstring& deviceId, AudioCallback callback);

    // Polling des notifications de changement (one-shot, auto-clear)
    bool hasDefaultDeviceChanged() { return m_defaultDeviceChanged.exchange(false); }
    bool hasDeviceListChanged()    { return m_deviceListChanged.exchange(false); }

    // Getters
    uint32_t     getSampleRate()       const { return m_sampleRate; }
    std::string  getDeviceName()       const { return m_deviceName; }
    std::wstring getCurrentDeviceId()  const { return m_currentDeviceId; }
    bool         isUsingDefaultDevice() const { return m_useDefaultDevice; }
    uint16_t     getChannels()        const { return m_channels; }
    uint16_t     getBitsPerSample()   const { return m_bitsPerSample; }
    bool         isFloatFormat()      const { return m_isFloat; }

private:
    friend class DeviceNotificationClient;

    // Boucle de capture exécutée dans m_thread
    void captureLoop();

    // Acquiert un device par ID (ou le défaut si deviceId vide).
    // Code commun à init() et switchDevice().
    bool acquireDevice(const std::wstring& deviceId);

    // Initialise les interfaces device (après que m_device est acquis).
    bool initDeviceInterfaces();

    // Libère les interfaces per-device (pas l'enumerator ni le notify client)
    void releaseDeviceInterfaces();

    // Convertit les données brutes WASAPI (multi-canal, format variable)
    // en vecteur mono float [-1, 1]
    std::vector<float> convertToMonoFloat(const BYTE* data, UINT32 numFrames);

    // --- Interfaces COM WASAPI ---
    IMMDeviceEnumerator* m_enumerator    = nullptr;
    IMMDevice*           m_device        = nullptr;
    IAudioClient*        m_audioClient   = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX*        m_waveFormat    = nullptr;

    // --- Notification ---
    DeviceNotificationClient* m_notifyClient = nullptr;
    std::atomic<bool> m_defaultDeviceChanged{false};
    std::atomic<bool> m_deviceListChanged{false};

    // --- Infos device ---
    uint32_t m_sampleRate    = 0;
    uint16_t m_channels      = 0;
    uint16_t m_bitsPerSample = 0;
    bool     m_isFloat       = false;
    std::string  m_deviceName;
    std::wstring m_currentDeviceId;
    bool m_useDefaultDevice = true;

    // --- Thread de capture ---
    std::atomic<bool> m_running{false};
    std::thread       m_thread;
    AudioCallback     m_callback;
};
