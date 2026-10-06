# Changelog

All notable changes to ReSkate Music Packer are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **Redesigned window**: a sidebar with the mod, a playlist list (replacing the tabs), a live cover
  preview with Image / Text / Auto, audio settings and a pinned Build button; a song table that reads
  like a list (frameless cells, artwork dots, compact row actions, a Problems column only when needed);
  drag a song by its number to reorder it or onto a playlist to move it there; a status bar for
  progress and what Build is waiting for; first-run setup as a stepped card. The custom theme that
  was defined but never applied is now used, with a bold heading font.
- The "is skate. running" check runs at most every two seconds instead of every frame.

### Added

- `--gui <files or mod folder>` (macOS/Linux) starts the GUI with those songs, or reopens that mod.
- `RSMP_CAPTURE=<png>` (macOS/Linux) saves one settled frame and exits, with
  `RSMP_CAPTURE_OPEN=settings|artwork|export|playlist` to open a dialog first: for checking the UI
  from scripts and CI.

### Added

- **macOS and Linux builds**: the same GUI (SDL2 instead of Win32/Direct3D 12) and CLI. The game's
  Oodle-compressed CAS data is read with the open-source ooz decoder (`External/ooz`); the mod's own
  CAS blocks are written uncompressed there. Native file dialogs (NSOpenPanel; zenity/kdialog), an
  app bundle on macOS, Homebrew/MacPorts on the PATH of a Finder-launched app, ReSkate's content
  cache found inside Proton/Wine prefixes, and case-insensitive lookup of the game's files on Linux.
- CI builds and tests macOS (Apple Silicon) and Linux (GCC and Clang) beside Windows.
- `cas_codec_tests`: Kraken/Selkie/Leviathan fixtures through `decode_cas()` off Windows.

### Changed

- ffmpeg/ffprobe run with an argument list (no command line to quote), so file names with quotes,
  `$` or backticks reach them unchanged on every platform.
- SHA-1/SHA-256 are computed in portable code (`src/sha.cpp`) instead of BCrypt.
- Platform code is split out: `platform_*.cpp`, `file_dialog_*`, `artwork_*.cpp`, `gui_renderer_*`.

## [1.1.2] - 2026-10-05

### Added

- **Recursive folder import**: dropping or choosing a folder now scans its subfolders for audio files, and duplicate paths are removed.

### Fixed

- **Case-only song titles collided in the bundle**: asset slugs are now deduplicated case-insensitively, so titles differing only in letter case (for example `Wit Em` vs `WIT EM`) get distinct assets (`..._wit_em_mg` / `..._wit_em_2_mg`) instead of overwriting each other.
- **Smart quotes in tags**: curly apostrophes and quotes (`’`, `‘`, `“`, `”`) and their double-encoded `â€™` forms are folded to ASCII before a song's artist/title becomes an asset path or song id.

## [1.1.1] - 2026-10-05

### Added

- **Thunderstore Export**:
  - Optional discreet *"Packaged with [ReSkate Music Packer]"* footer link in auto-generated READMEs, with a toggle checkbox in the export dialog and a `--no-readme-credit` CLI flag.

## [1.1.0] - 2026-10-05

### Added

- **Multi-playlist management suite**:
  - Filter track table by playlist using interactive playlist tabs with track count badges (`All`, `Default`, custom playlists, and a `+` new playlist button).
  - Per-track playlist assignment dropdown button in the table to quickly switch playlists or create a new playlist.
  - Track context menu (right-click anywhere across track row or click the `...` action button) with quick move-to-playlist, reordering, artwork, and removal options.
  - Empty table right-click context menu to quickly add songs, folders, create playlists, or manage artwork.
  - Multi-playlist duration and song count breakdown tooltip when hovering over the table footer summary.
  - Multi-playlist cover manager popup (`Covers (N)` in the toolbar) with playlist selection, active status badges, live 128x128 preview container, and 1-click batch generation.
- **Modernized UI**:
  - Elevated dark header toolbar ribbon with rounded borders and centered controls.
  - Re-imagined hero empty state dropzone card with a 9-bar vector audio waveform equalizer graphic, centered layout, and quick-add actions.
  - Real-time footer summary displaying total playlist count, song count, and formatted playback duration.

### Fixed

- **Direct3D 12 texture crash on cover re-generation**: Deferred texture resource release until GPU fence completion, preventing device removal crashes (`DXGI_ERROR_DEVICE_REMOVED` / page fault) when re-generating text covers or changing images while draw calls from previous frames are still in flight.
- **Preview persistence in covers popup**: Fixed cover previews not updating or disappearing when switching between playlists in the toolbar covers dropdown.
- **Missing font glyphs**: Replaced unicode bullet `•` in the footer summary with an ASCII pipe ` | ` and replaced unicode dropdown arrows with native `ImGui::ArrowButton` to prevent `?` characters from displaying.
- **Modal race conditions**: Action buttons in `Playlist Artwork Setup` and `PlaylistCoverPopup` are now disabled while artwork generation or decoding is active.

## [1.0.1] - 2026-10-05

### Fixed

- Thunderstore icon picker now filters for images (`.png`, `.jpg`, `.jpeg`, `.webp`, `.bmp`) by default instead of audio files.
- Thunderstore custom icons are now scaled and converted to standard 256x256 PNGs.
- Fixed File Explorer auto-reveal failing silently when exporting a Thunderstore package.
- Exclude local editor project files (`reskate-music-project.json`) from Thunderstore export packages.

### Added

- Thunderstore export dialog now allows selecting a custom output folder, previews the target package path, and displays the full export path in the status bar.
- Inline playlist cover selector button directly beside the playlist name field, with 1-click options to choose an image, generate a text cover, or reset to automatic.
- Pre-build confirmation prompt with live visual cover preview when playlist artwork is still set to Automatic, allowing 1-click generation of text covers, picking an image, or continuing with automatic.
- Clearer signposting for multi-playlist creation, including descriptive tooltips on the default playlist input, table column header, and per-track playlist cells.

## [1.0.0] - 2026-10-05

### Added

- Cover artwork: extract embedded album art automatically, choose a track or playlist image, or
  generate a text cover. Covers are 512x512 PNGs saved with the mod and included in Thunderstore
  exports.
- Documentation: this changelog, a `CONTRIBUTING` guide, a rewritten README, and guides under `docs/`,
  plus a `LICENSE`.
- A **Settings** dialog (from the toolbar) to change the game folder and ffmpeg after the first-run
  setup, instead of only at startup.
- **Download ffmpeg automatically**: when ffmpeg/ffprobe are missing, the setup page and the Settings
  dialog can fetch a pinned static LGPL build (BtbN/FFmpeg-Builds) into `.\ffmpeg` beside the app or
  `%LOCALAPPDATA%\ReSkateMusicPacker\ffmpeg`. Only a click starts it; the archive is verified against
  a pinned SHA-256 before extraction, and `FFMPEG_URL` / `FFMPEG_SHA256` override the source. The CLI
  gets `--get-ffmpeg [folder]` for the same, headlessly.

### Changed

- **Relicensed from MIT to GPL-3.0**, with attribution: `src/Engine/` is ported from the
  [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) project's engine code (Copyright © 2026 the
  ReSkate contributors, GPL-3.0). See `NOTICE.md`.
- Loudness normalisation now targets **-15 LUFS** with two-pass `loudnorm`, matching the level of the
  game's own tracks (they measure about -15 LUFS), instead of -16 with a single dynamic pass.
- ffmpeg/ffprobe now run without a console window, and with their output captured in-process, so the
  GUI no longer flashes command prompts.
- The Thunderstore export builds its `.zip` in-process (miniz), so there is no dependency on the
  system `tar`.

### Fixed

- Write the correct sample count in the stream's codec header, so a mod track reaches its end and the
  game advances to the next track instead of stopping.

## [0.1.0] - 2026-10-04

### Added

- First release: a hybrid Dear ImGui / DirectX 12 GUI and a scriptable CLI.
- Add-only music mods: Opus encoding with a per-source encode cache and EBU R128 loudness
  normalisation.
- Multiple playlists per mod, and a warning when a song's id clashes with an installed or built-in
  song.
- Thunderstore export: a ready `.zip` with `manifest.json`, a generated README tracklist, and a
  generated or custom icon.
- High-DPI aware interface.
