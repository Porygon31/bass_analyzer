# 🎵 BassAnalyzer v0.2

Analyseur de fréquences basses en temps réel avec GUI. Capture le son interne du PC (WASAPI loopback), détecte la fréquence dominante des basses via FFT + autocorrélation, avec visualisation spectrale ImGui/DirectX11.

## Fonctionnalités

- **Visualisation spectrale temps réel** — barres + courbe avec smoothing configurable
- **Historique temporel** — graphe scrollant des fréquences sur ~30 secondes
- **Détection de pitch par autocorrélation** (McLeod NSDF) — plus précis que la FFT seule pour les basses
- **Tuner intégré** — affichage note + cents + indicateur de justesse
- **VU-mètre dB** avec gradient de couleur
- **Cutoff ajustable** en temps réel (30-500 Hz)
- **Double mode** : GUI (par défaut) et Console (debug)

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

# GUI (fenêtre DirectX11 + ImGui)
cmake -DFFTW3_DIR=C:\libs\fftw-3.3.5-dll64 ..
cmake --build . --target bass_analyzer --config Release

# Console (mode texte, debug)
cmake --build . --target bass_analyzer_console --config Release
```

## Utilisation

```cmd
# Mode GUI (par défaut)
bass_analyzer.exe

# Mode Console
bass_analyzer_console.exe
```

### Contrôles GUI

| Élément | Action |
|---------|--------|
| Slider "Bass Cutoff" | Ajuster la fréquence de coupure (30-500 Hz) |
| Slider "Smoothing" | Lissage du spectre (0 = brut, 0.95 = très lissé) |
| Checkboxes | Afficher/masquer Spectrum, History, Pitch |
| Panels | Repliables via les headers |

### Contrôles Console

| Touche | Action |
|--------|--------|
| `Q` / `ESC` | Quitter |
| `+` / `-` | Ajuster le cutoff (±10Hz) |
| `R` | Reset cutoff à 100Hz |

## Structure des fichiers

```
src/
├── main.cpp            # Point d'entrée (GUI / Console selon le define)
├── gui_app.h/cpp       # Application ImGui + DirectX 11
├── audio_capture.h/cpp # WASAPI loopback capture
├── bass_detector.h/cpp # FFT, analyse spectrale, historique
├── pitch_detector.h/cpp # Autocorrélation McLeod NSDF
└── dsp_utils.h/cpp     # Butterworth, décimation, fenêtrage
```

## Améliorations futures

- [ ] Export des données (CSV, OSC pour VJing / mapping)
- [ ] Sélection du device audio dans la GUI
- [ ] Mode multiband (sub-bass / mid-bass / upper-bass)
- [ ] Waterfall / spectrogram 2D (temps × fréquence × magnitude)
- [ ] Custom font pour les gros affichages Hz
- [ ] Thèmes de couleur
