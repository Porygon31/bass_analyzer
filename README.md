# 🎵 BassAnalyzer v0.2

Analyseur de fréquences basses en temps réel avec GUI. Capture le son interne du PC (WASAPI loopback), détecte la fréquence dominante des basses via FFT + autocorrélation, avec visualisation spectrale ImGui/DirectX11.

## Fonctionnalités

- **Visualisation spectrale temps réel** — barres + courbe avec smoothing configurable
- **Historique temporel** — graphe scrollant des fréquences sur ~30 secondes
- **Détection de pitch par autocorrélation** (McLeod NSDF) — plus précis que la FFT seule pour les basses
- **Tuner intégré** — affichage note + cents + indicateur de justesse
- **Oscilloscope** — forme d'onde brute en temps réel (~400ms)
- **VU-mètre dB** avec gradient de couleur
- **Sélection du device audio** — switch entre les sorties audio en temps réel
- **Cutoff ajustable** en temps réel (30-500 Hz)
- **Personnalisation des couleurs** du spectre (4 niveaux d'intensité)
- **Guide d'utilisation intégré** — menu Aide avec explication de chaque panneau
- **Fenêtre redimensionnable** avec gestion correcte du DPI

## Architecture / Pipeline

```
Audio Loopback (48kHz, WASAPI)
    │
    ├─► Conversion Mono Float (supporte 16/24/32bit, N canaux)
    │
    ├─► Filtre Passe-Bas Butterworth 4ème ordre
    │
    ├─► Décimation (48kHz → ~400Hz)  ← ~120x moins de données
    │
    ├─► FFT (FFTW3, fenêtre Hann, 2s, ~0.5Hz résolution)
    │   ├─► Interpolation parabolique sub-bin
    │   ├─► Spectre → GUI
    │   └─► Peak fréquence → Historique
    │
    └─► Autocorrélation NSDF (McLeod)
        ├─► Interpolation parabolique sub-sample
        ├─► Détection de note musicale
        └─► Confiance de détection
```

## Dépendances

- **Windows 10/11** (WASAPI + DirectX 11)
- **CMake** >= 3.20
- **FFTW3** (précompilé pour Windows)
- **Visual Studio** 2022 ou **MinGW-w64** (C++20)
- **ImGui** v1.91.8 (téléchargé automatiquement par CMake via FetchContent)

## Installation FFTW3

1. Télécharger depuis https://www.fftw.org/install/windows.html (64-bit DLL)
2. Extraire dans un dossier, ex: `C:\libs\fftw-3.3.5-dll64`
3. Générer le `.lib` pour MSVC :
   ```cmd
   cd C:\libs\fftw-3.3.5-dll64
   lib /machine:x64 /def:libfftw3-3.def
   ```

## Build

```cmd
mkdir build && cd build
cmake -DFFTW3_DIR=C:\libs\fftw-3.3.5-dll64 ..
cmake --build . --target bass_analyzer --config Release
```

## Utilisation

```cmd
bass_analyzer.exe
```

### Contrôles GUI

| Élément | Action |
|---------|--------|
| Combo "Device" | Sélectionner la sortie audio à analyser |
| Slider "Bass Cutoff" | Ajuster la fréquence de coupure (30-500 Hz) |
| Combo "FFT Size" | Résolution fréquentielle (Auto / 512 / 1024 / ... / 16384) |
| Slider "Smoothing" | Lissage du spectre (0 = brut, 0.95 = très lissé) |
| Checkboxes | Afficher/masquer Spectrum, History, Pitch, Oscilloscope |
| Menu Aide | Guide d'utilisation intégré |
| Menu Settings | Personnalisation des couleurs du spectre (4 niveaux d'intensité) |
| Panels | Repliables via les headers |

## Structure des fichiers

```
bass_analyzer/
├── CMakeLists.txt          # Configuration CMake (C++20, FetchContent ImGui)
├── README.md
├── main.cpp                # Point d'entrée (GUI / Console selon le define)
├── gui_app.h/cpp           # Application ImGui + DirectX 11
├── audio_capture.h/cpp     # WASAPI loopback capture
├── bass_detector.h/cpp     # FFT, analyse spectrale, historique
├── pitch_detector.h/cpp    # Autocorrélation McLeod NSDF
└── dsp_utils.h/cpp         # Butterworth, décimation, fenêtrage
```

## Améliorations futures

- [ ] Export des données (CSV, OSC pour VJing / mapping)
- [ ] Mode multiband (sub-bass / mid-bass / upper-bass)
- [ ] Waterfall / spectrogram 2D (temps × fréquence × magnitude)
- [ ] Custom font pour les gros affichages Hz
- [x] ~~Sélection du device audio dans la GUI~~
- [x] ~~Thèmes de couleur~~ (couleurs du spectre configurables)
