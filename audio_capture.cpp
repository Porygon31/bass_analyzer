/*
 * audio_capture.cpp — Implémentation de la capture WASAPI loopback
 *
 * Fonctionnement :
 * 1. On récupère le device audio par défaut (eRender = sortie speakers)
 *    ou un device spécifique par son ID
 * 2. On l'initialise en mode AUDCLNT_STREAMFLAGS_LOOPBACK
 *    → ça dit à Windows "je veux lire ce qui sort, pas ce qui entre"
 * 3. Un thread tourne en boucle et lit les paquets audio disponibles
 * 4. Chaque paquet est converti en mono float et envoyé au callback
 *
 * Détection des changements :
 * - IMMNotificationClient reçoit les notifications de Windows quand
 *   un device est ajouté/retiré ou quand le device par défaut change.
 * - Des flags atomiques sont set pour que la GUI puisse réagir.
 *
 * Formats supportés : 16-bit int, 24-bit int, 32-bit float (le plus courant)
 * Le downmix stéréo→mono se fait par simple moyenne des canaux.
 */
#include "audio_capture.h"
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <cstring>

// Macro pour libérer proprement les interfaces COM
#define SAFE_RELEASE(p) do { if(p) { (p)->Release(); (p) = nullptr; } } while(0)

// Macro pour vérifier un HRESULT et retourner false avec un message d'erreur
#define CHECK_HR(hr, msg) do { \
    if(FAILED(hr)) { \
        std::cerr << "[AudioCapture] " << msg << " (0x" << std::hex << hr << std::dec << ")\n"; \
        return false; \
    } \
} while(0)

// =============================================================================
// DeviceNotificationClient — Implémentation COM IMMNotificationClient
// =============================================================================

DeviceNotificationClient::DeviceNotificationClient(AudioCapture* owner)
    : m_owner(owner) {}

ULONG STDMETHODCALLTYPE DeviceNotificationClient::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE DeviceNotificationClient::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
        *ppv = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDefaultDeviceChanged(
    EDataFlow flow, ERole role, LPCWSTR /*pwstrDeviceId*/)
{
    // On ne s'intéresse qu'aux changements de device de sortie (eRender)
    // pour le rôle console (musique, jeux, etc.)
    if (flow == eRender && role == eConsole) {
        m_owner->m_defaultDeviceChanged.store(true);
        m_owner->m_deviceListChanged.store(true);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) {
    m_owner->m_deviceListChanged.store(true);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) {
    m_owner->m_deviceListChanged.store(true);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceStateChanged(
    LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/)
{
    m_owner->m_deviceListChanged.store(true);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnPropertyValueChanged(
    LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/)
{
    return S_OK;
}

// =============================================================================
// AudioCapture
// =============================================================================

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    stop();

    // Désenregistrer le client de notification avant de libérer l'enumerator
    if (m_enumerator && m_notifyClient) {
        m_enumerator->UnregisterEndpointNotificationCallback(m_notifyClient);
    }
    if (m_notifyClient) {
        m_notifyClient->Release();
        m_notifyClient = nullptr;
    }

    // Libération des ressources per-device
    releaseDeviceInterfaces();

    // Libération de l'enumerator et COM
    SAFE_RELEASE(m_enumerator);
    CoUninitialize();
}

void AudioCapture::releaseDeviceInterfaces() {
    if (m_waveFormat) { CoTaskMemFree(m_waveFormat); m_waveFormat = nullptr; }
    SAFE_RELEASE(m_captureClient);
    SAFE_RELEASE(m_audioClient);
    SAFE_RELEASE(m_device);

    m_sampleRate = 0;
    m_channels = 0;
    m_bitsPerSample = 0;
    m_isFloat = false;
    m_deviceName.clear();
}

bool AudioCapture::init(const std::wstring& deviceId) {
    HRESULT hr;

    // Initialisation COM en mode multithread (nécessaire pour WASAPI)
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CHECK_HR(hr, "CoInitializeEx failed");

    // Création de l'énumérateur de devices audio Windows
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator
    );
    CHECK_HR(hr, "Failed to create device enumerator");

    // Enregistrement du client de notification
    m_notifyClient = new DeviceNotificationClient(this);
    hr = m_enumerator->RegisterEndpointNotificationCallback(m_notifyClient);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Warning: failed to register notification callback\n";
    }

    // Acquisition du device
    if (deviceId.empty()) {
        hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        CHECK_HR(hr, "Failed to get default audio endpoint");
        m_useDefaultDevice = true;
    } else {
        hr = m_enumerator->GetDevice(deviceId.c_str(), &m_device);
        CHECK_HR(hr, "Failed to get audio device by ID");
        m_useDefaultDevice = false;
    }

    // Stocker l'ID du device courant
    LPWSTR devId = nullptr;
    if (SUCCEEDED(m_device->GetId(&devId))) {
        m_currentDeviceId = devId;
        CoTaskMemFree(devId);
    }

    return initDeviceInterfaces();
}

bool AudioCapture::initDeviceInterfaces() {
    HRESULT hr;

    // Lecture du nom "friendly" du device (ex: "Speakers (Realtek HD Audio)")
    IPropertyStore* props = nullptr;
    hr = m_device->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            // Conversion wide string → UTF-8
            int size = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
            m_deviceName.resize(size - 1);
            WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, m_deviceName.data(), size, nullptr, nullptr);
        }
        PropVariantClear(&varName);
        props->Release();
    }

    // Activation du client audio sur le device
    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    CHECK_HR(hr, "Failed to activate audio client");

    // Récupération du format de mixage du device
    // C'est le format natif de la sortie audio (généralement 48kHz 32-bit float stéréo)
    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    CHECK_HR(hr, "Failed to get mix format");

    m_sampleRate    = m_waveFormat->nSamplesPerSec;
    m_channels      = m_waveFormat->nChannels;
    m_bitsPerSample = m_waveFormat->wBitsPerSample;

    // Détection du format d'échantillon (float ou int)
    // Windows moderne utilise presque toujours WAVE_FORMAT_EXTENSIBLE + IEEE_FLOAT
    if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        m_isFloat = true;
    } else if (m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_waveFormat);
        m_isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    // Initialisation du client audio en mode loopback
    // AUDCLNT_STREAMFLAGS_LOOPBACK : capture ce qui sort des speakers
    // Buffer de 50ms (500000 unités de 100ns) — compromis latence/stabilité
    REFERENCE_TIME bufferDuration = 500000; // 50ms
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,        // Mode partagé (pas d'accès exclusif)
        AUDCLNT_STREAMFLAGS_LOOPBACK,    // Mode loopback !
        bufferDuration, 0,
        m_waveFormat, nullptr
    );
    CHECK_HR(hr, "Failed to initialize audio client in loopback mode");

    // Obtention de l'interface de capture (pour lire les buffers)
    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    CHECK_HR(hr, "Failed to get capture client");

    std::cout << "[AudioCapture] Device: " << m_deviceName << "\n";
    std::cout << "[AudioCapture] Format: " << m_sampleRate << " Hz, "
              << m_channels << " ch, " << m_bitsPerSample << " bit"
              << (m_isFloat ? " float" : " int") << "\n";

    return true;
}

bool AudioCapture::start(AudioCallback callback) {
    if (m_running) return false;
    m_callback = std::move(callback);

    // Démarre le flux audio (WASAPI commence à remplir le buffer)
    HRESULT hr = m_audioClient->Start();
    CHECK_HR(hr, "Failed to start audio client");

    // Lance le thread de capture
    m_running = true;
    m_thread = std::thread(&AudioCapture::captureLoop, this);
    return true;
}

void AudioCapture::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_audioClient) m_audioClient->Stop();
}

std::vector<AudioDeviceInfo> AudioCapture::enumerateDevices() {
    std::vector<AudioDeviceInfo> result;
    if (!m_enumerator) return result;

    // Récupérer l'ID du device par défaut pour le marquer
    IMMDevice* defaultDev = nullptr;
    std::wstring defaultId;
    if (SUCCEEDED(m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDev))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDev->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
        defaultDev->Release();
    }

    // Énumérer tous les devices de sortie actifs
    IMMDeviceCollection* collection = nullptr;
    if (FAILED(m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
        return result;

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(collection->Item(i, &dev))) continue;

        AudioDeviceInfo info;

        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }

        IPropertyStore* props = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
                int size = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                info.name.resize(size - 1);
                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, info.name.data(), size, nullptr, nullptr);
            }
            PropVariantClear(&varName);
            props->Release();
        }

        info.isDefault = (info.id == defaultId);
        result.push_back(std::move(info));
        dev->Release();
    }
    collection->Release();
    return result;
}

bool AudioCapture::switchDevice(const std::wstring& deviceId, AudioCallback callback) {
    // 1. Arrêter la capture
    stop();

    // 2. Libérer les interfaces per-device (pas l'enumerator ni le notify client)
    releaseDeviceInterfaces();

    // 3. Acquérir le nouveau device
    HRESULT hr;
    if (deviceId.empty()) {
        hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        m_useDefaultDevice = true;
    } else {
        hr = m_enumerator->GetDevice(deviceId.c_str(), &m_device);
        m_useDefaultDevice = false;
    }
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to get device for switch (0x"
                  << std::hex << hr << std::dec << ")\n";
        return false;
    }

    // Stocker l'ID du nouveau device
    LPWSTR devId = nullptr;
    if (SUCCEEDED(m_device->GetId(&devId))) {
        m_currentDeviceId = devId;
        CoTaskMemFree(devId);
    }

    // 4. Initialiser les interfaces sur le nouveau device
    if (!initDeviceInterfaces()) return false;

    // 5. Redémarrer la capture
    return start(std::move(callback));
}

void AudioCapture::captureLoop() {
    while (m_running) {
        // On dort 10ms pour laisser le buffer se remplir
        // À 48kHz, 10ms ≈ 480 échantillons — assez pour le traitement
        Sleep(10);

        // Vérifie combien de paquets sont disponibles dans le buffer
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        // Traite tous les paquets disponibles
        while (packetLength > 0) {
            BYTE*  data      = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags     = 0;

            // Récupère un pointeur vers les données du buffer
            hr = m_captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            // AUDCLNT_BUFFERFLAGS_SILENT = le buffer est silencieux (pas de son)
            // On skip dans ce cas pour économiser du CPU
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && numFrames > 0) {
                auto monoSamples = convertToMonoFloat(data, numFrames);
                if (m_callback && !monoSamples.empty()) {
                    m_callback(monoSamples, m_sampleRate);
                }
            }

            // IMPORTANT : toujours libérer le buffer après lecture
            m_captureClient->ReleaseBuffer(numFrames);

            // Vérifie s'il reste d'autres paquets
            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }
}

std::vector<float> AudioCapture::convertToMonoFloat(const BYTE* data, UINT32 numFrames) {
    std::vector<float> mono(numFrames);

    // === 32-bit IEEE Float (le plus courant sous Windows 10/11) ===
    // Les samples sont déjà en float [-1.0, 1.0], on fait juste le downmix mono
    if (m_isFloat && m_bitsPerSample == 32) {
        const float* src = reinterpret_cast<const float*>(data);
        for (UINT32 i = 0; i < numFrames; i++) {
            float sum = 0.0f;
            for (uint16_t ch = 0; ch < m_channels; ch++) {
                sum += src[i * m_channels + ch];
            }
            mono[i] = sum / static_cast<float>(m_channels); // Moyenne des canaux
        }
    }
    // === 16-bit signed integer (PCM classique) ===
    // Range [-32768, 32767] → normalisé en [-1.0, 1.0]
    else if (!m_isFloat && m_bitsPerSample == 16) {
        const int16_t* src = reinterpret_cast<const int16_t*>(data);
        for (UINT32 i = 0; i < numFrames; i++) {
            float sum = 0.0f;
            for (uint16_t ch = 0; ch < m_channels; ch++) {
                sum += static_cast<float>(src[i * m_channels + ch]) / 32768.0f;
            }
            mono[i] = sum / static_cast<float>(m_channels);
        }
    }
    // === 24-bit signed integer (qualité studio) ===
    // Pas de type natif 24-bit en C++, on reconstitue manuellement
    // 3 octets par sample, little-endian, sign-extend vers 32-bit
    else if (!m_isFloat && m_bitsPerSample == 24) {
        for (UINT32 i = 0; i < numFrames; i++) {
            float sum = 0.0f;
            for (uint16_t ch = 0; ch < m_channels; ch++) {
                size_t offset = (i * m_channels + ch) * 3;
                // Reconstitution du int24 en int32
                int32_t sample = (data[offset] | (data[offset+1] << 8) | (data[offset+2] << 16));
                if (sample & 0x800000) sample |= 0xFF000000; // Extension de signe
                sum += static_cast<float>(sample) / 8388608.0f; // 2^23
            }
            mono[i] = sum / static_cast<float>(m_channels);
        }
    }

    return mono;
}
