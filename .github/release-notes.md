## Downloads

| System | File |
|---|---|
| Windows 10/11 (x64) | `...-win64.zip`: unzip and run `ReSkateMusicPacker.exe` |
| macOS 11+ (Apple Silicon and Intel) | `...-macos-universal.zip`: unzip and open `ReSkateMusicPacker.app` |
| Linux x86-64 (Ubuntu 22.04+, Fedora, Arch, SteamOS, ...) | `...-linux-x86_64.tar.gz`: extract and run `./ReSkateMusicPacker` |

You also need **ffmpeg** (Windows: the app can download it for you; macOS: `brew install ffmpeg`;
Linux: your package manager, e.g. `sudo apt install ffmpeg`).

**macOS:** the app is not notarized, so the first launch is blocked ("cannot be opened" or "is
damaged"). Right-click it and choose **Open**, or run
`xattr -dr com.apple.quarantine ReSkateMusicPacker.app` once.

**Linux:** needs SDL2 (`sudo apt install libsdl2-2.0-0`, `sudo dnf install SDL2`, `sudo pacman -S sdl2`);
the file pickers use `zenity` or `kdialog` if installed, and dragging files onto the window always works.

On macOS and Linux the game itself runs under Proton, Wine or CrossOver: point the packer at that
install folder (the one with `Skate.exe`). Mods built there store their archive uncompressed (no
Oodle encoder outside Windows).

> **Pre-release:** the macOS and Linux builds have not yet been tested against the real game. Please
> report whether mods built with them load in game.
