# SpectraVisual

Applicazione desktop per visualizzare spettri rotazionali sperimentali e
previsioni Pickett/SPCAT (`.cat`). Il progetto include già ImGui e ImPlot:
l'unica dipendenza esterna è **SDL2** (con SDL2_ttf).

## Compilazione rapida

Aprire un terminale nella cartella del progetto e usare i comandi della propria
piattaforma. Il risultato è `spectravisual` (`spectravisual.exe` su Windows).

### Linux

**Debian / Ubuntu**

```sh
sudo apt update
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev
make
```

**Fedora**

```sh
sudo dnf install gcc gcc-c++ make SDL2-devel SDL2_ttf-devel
make
```

**Arch Linux**

```sh
sudo pacman -S --needed base-devel sdl2 sdl2_ttf
make
```

### macOS

Richiede gli strumenti a riga di comando di Xcode e [Homebrew](https://brew.sh).

```sh
xcode-select --install       # solo la prima volta
brew install sdl2 sdl2_ttf
make
```

Funziona sia su Mac Apple Silicon sia su Mac Intel. Se Homebrew non viene
rilevato automaticamente, seguire il fallback SDL2 commentato nel `makefile`.

### Windows (MSYS2 / UCRT64)

1. Installare [MSYS2](https://www.msys2.org/) e aprire **MSYS2 UCRT64**,
   non PowerShell o il prompt di Windows.
2. Aggiornare l'ambiente e installare compilatore e librerie:

```sh
pacman -Syu
pacman -S --needed base-devel mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-SDL2_ttf
```

3. Entrare nella cartella del progetto (per esempio `cd /c/Users/nome/liveplot`)
   e compilare:

```sh
make
```

L'eseguibile è `spectravisual.exe`. Per distribuirlo fuori da MSYS2, copiare
accanto ad esso le DLL di SDL2 presenti in `C:/msys64/ucrt64/bin`.

## Avvio e comandi utili

```sh
./spectravisual                         # finestra vuota
./spectravisual spettro.csv modello.cat # dati sperimentali + previsione
make clean                              # elimina gli artefatti di build
make test                               # esegue i test di regressione
```

L'app accetta più file di spettro e una previsione `.cat`, in qualsiasi
ordine. Su Windows usare `./spectravisual.exe`.

## Requisiti

- compilatore C e C++ (`gcc`/`g++` o equivalenti);
- `make`;
- SDL2 e SDL2_ttf in versione di sviluppo.

## Licenza e uso

Software per ricerca e sviluppo: verificare autonomamente i risultati prima di
usarli per analisi o pubblicazioni.
