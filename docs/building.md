# Building from source

## Prerequisites

- **Windows**: **Visual Studio 2022** with the **Desktop development with C++** workload (v143 toolset).
- **macOS**: Xcode command line tools (Apple Clang) and Homebrew's `sdl2` (`brew install sdl2 ffmpeg`).
- **Linux**: GCC 12+ or Clang 16+, and SDL2's development package (`sudo apt install build-essential
  cmake libsdl2-dev ffmpeg`, `sudo dnf install gcc-c++ cmake SDL2-devel ffmpeg`, ...). The file
  pickers use `zenity` or `kdialog` when present; dragging files onto the window always works.
- **CMake 3.20+** everywhere.
- Everything else is vendored under `External/`: Dear ImGui, zstd, LZ4, miniz and RapidJSON, plus,
  for macOS/Linux, ooz (Oodle decoder) with a slice of SIMD Everywhere, and stb_image.

## Build on Windows

From the repository root:

```powershell
cmake -B build/vs2022-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022-x64 --config Release
```

Outputs land in `build/vs2022-x64/Release/`:

- `ReSkateMusicPacker.exe` - the unified GUI & CLI application.
- `ReSkateMusicPackerTests.exe` - the regression tests.

## Build on macOS and Linux

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

- macOS: `build/ReSkateMusicPacker.app` (the CLI is `build/ReSkateMusicPacker.app/Contents/MacOS/ReSkateMusicPacker`).
- Linux: `build/ReSkateMusicPacker`.
- `-DRSMP_WARNINGS_AS_ERRORS=ON` makes warnings errors, as CI builds do (MSVC builds always do).

```sh
ctest --test-dir build --output-on-failure
```

To look at the GUI without a screen (CI, scripts, review), `RSMP_CAPTURE=shot.png` saves a frame and
exits, and `--gui <songs or a mod folder>` fills it first:

```sh
RSMP_CAPTURE=songs.png build/ReSkateMusicPacker --gui ~/Music/*.mp3
RSMP_CAPTURE=export.png RSMP_CAPTURE_OPEN=export build/ReSkateMusicPacker --gui "<game>/Mods/MyMix"
```

The extra `cas_codec_tests` check the Oodle path there: Kraken, Selkie and Leviathan blocks (made
from this repository's `LICENSE`, in `test/fixtures/`) decode through `decode_cas()` and ooz.

## Tests

```powershell
.\build\vs2022-x64\Release\ReSkateMusicPackerTests.exe
```

It prints `all passed` on success. The tests cover packing and the artwork conversion, and need
ffmpeg/ffprobe on `PATH`.

## Layout of the source

```
src/
  main.cpp            # GUI (Dear ImGui) and the CLI entry point; Win32 window or SDL2 window
  packer.cpp/.h       # the library: scan, encode, build the mod, Thunderstore export
  artwork_*.cpp       # the generated text cover: GDI+WIC (win32) or stb_truetype+miniz (posix)
  platform*.{h,cpp}   # processes, PATH, per-user folders, opening URLs/folders, per OS
  file_dialog*        # the open dialog: Explorer, NSOpenPanel (mac .mm) or zenity/kdialog (linux)
  sha.cpp/.h          # SHA-1 / SHA-256
  Engine/              # Frostbite RES/EBX/TOC/cas readers, writer, and bundle handling
  gui_renderer*       # the GUI renderer: Direct3D 12 (win32) or SDL_Renderer (sdl)
External/             # vendored third-party libraries
test/                 # tests
```
