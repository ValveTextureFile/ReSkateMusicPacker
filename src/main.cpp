// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// ReSkateMusicMaker: a window over the music packer library (MusicPacker/packer.h). Songs in a
// list (dropped or added), their artist and title editable, then built into the game's Mods
// folder. A mod built here can be opened again from its project file and rebuilt.
// Drawn with Dear ImGui through Direct3D 12 on Windows (gui_renderer_win32.cpp) and SDL2 elsewhere
// (gui_renderer_sdl.cpp).
#include "packer.h"
#include "ffmpeg_fetch.h"
#include "file_dialog.h"
#include "platform.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_case.h"
#include "gui_renderer.h"

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#else
#include <SDL.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>
#endif
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#endif

namespace {
namespace fs = std::filesystem;
using dingosdk::Json;
using dingosdk::launcher_gui::detail::NativeWindow;
using dingosdk::launcher_gui::detail::Renderer;
using platform::narrow;
using platform::widen;

constexpr int window_width = 1100, window_height = 700;
constexpr const char* bitrates[]{"128", "160", "192", "256", "320"};

float g_scale = 1.0f;
inline float S(float value) { return value * g_scale; }

// ---- Look and feel: the palette and small widgets the pages share ----------------------------------
namespace ui {
const ImVec4 accent(0.22f, 0.58f, 0.98f, 1.00f);
const ImVec4 accent_hover(0.30f, 0.65f, 1.00f, 1.00f);
const ImVec4 accent_active(0.16f, 0.48f, 0.86f, 1.00f);
const ImVec4 danger(0.93f, 0.36f, 0.33f, 1.00f);
const ImVec4 success(0.40f, 0.85f, 0.52f, 1.00f);
const ImVec4 warning(1.00f, 0.70f, 0.30f, 1.00f);
const ImVec4 muted(0.50f, 0.54f, 0.60f, 1.00f);
const ImVec4 panel(0.135f, 0.145f, 0.175f, 1.00f);
const ImVec4 transparent(0, 0, 0, 0);

ImFont* heading_font = nullptr; // set by run_gui when a bold face is available

void heading(const char* text) {
    if (heading_font) ImGui::PushFont(heading_font);
    ImGui::TextUnformatted(text);
    if (heading_font) ImGui::PopFont();
}
// A small upper-case label over a group of controls.
void section(const char* text) {
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}
bool primary_button(const char* label, ImVec2 size = {}) {
    ImGui::PushStyleColor(ImGuiCol_Button, accent_active);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}
// A borderless button that only shows its background on hover (row actions, icons).
bool ghost_button(const char* label, ImVec2 size = {}, const ImVec4* hover = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Button, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover ? *hover : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    // Shorter than a frame (header buttons): drop the vertical padding so the label isn't clipped.
    const bool short_button = size.y > 0 && size.y < ImGui::GetFrameHeight();
    if (short_button) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0));
    const bool clicked = ImGui::Button(label, size);
    if (short_button) ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return clicked;
}
void status_text(const std::string& text, bool error) {
    ImGui::TextColored(error ? danger : success, "%s", text.c_str());
}
// "3:07", or "1:02:03" past an hour.
std::string duration(double seconds) {
    const int total = static_cast<int>(seconds);
    char text[32];
    if (total >= 3600) std::snprintf(text, sizeof(text), "%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60);
    else std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}
// Shortens text to `width` by cutting the middle, so both ends of a path stay readable.
std::string fit_middle(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::size_t keep = text.size();
    while (keep > 4) {
        keep -= 1;
        const auto candidate = text.substr(0, keep / 2) + "..." + text.substr(text.size() - (keep - keep / 2));
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
    }
    return "...";
}
// Shortens text to `width` by cutting its end ("needs an art...").
std::string fit_end(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    for (std::size_t keep = text.size(); keep > 0; --keep) {
        const auto candidate = text.substr(0, keep) + "...";
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) return candidate;
    }
    return "...";
}
// A rounded panel centred in the space left, for the first-run pages and the empty song list.
bool begin_card(const char* id, ImVec2 size) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    size.x = std::min(size.x, avail.x - S(32));
    size.y = std::min(size.y, avail.y - S(16));
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - size.x) * 0.5f),
                               ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - size.y) * 0.45f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.30f, 0.40f, 0.70f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(32.0f), S(26.0f)));
    return ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}
void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
// "(1) Game folder -- (2) ffmpeg -- (3) Songs", done steps ticked, the current one in the accent colour.
void steps(int current) {
    const char* names[]{"Game folder", "ffmpeg", "Songs"};
    auto* draw = ImGui::GetWindowDrawList();
    const float radius = S(11.0f);
    for (int i = 0; i < 3; ++i) {
        if (i) {
            ImGui::SameLine(0, S(10));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float y = at.y + radius;
            draw->AddLine(ImVec2(at.x, y), ImVec2(at.x + S(28), y), ImGui::GetColorU32(muted), S(1.5f));
            ImGui::Dummy(ImVec2(S(28), radius * 2));
            ImGui::SameLine(0, S(10));
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 centre(at.x + radius, at.y + radius);
        const bool done = i < current, now = i == current;
        if (done || now) draw->AddCircleFilled(centre, radius, ImGui::GetColorU32(done ? success : accent));
        else draw->AddCircle(centre, radius, ImGui::GetColorU32(muted), 0, S(1.5f));
        if (done) { // a tick
            draw->AddPolyline(std::array<ImVec2, 3>{ImVec2(centre.x - radius * 0.45f, centre.y),
                                                    ImVec2(centre.x - radius * 0.1f, centre.y + radius * 0.35f),
                                                    ImVec2(centre.x + radius * 0.45f, centre.y - radius * 0.35f)}.data(),
                              3, IM_COL32_WHITE, 0, S(2.0f));
        } else {
            const char digit[2]{static_cast<char>('1' + i), 0};
            const ImVec2 size = ImGui::CalcTextSize(digit);
            draw->AddText(ImVec2(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f),
                          now ? IM_COL32_WHITE : ImGui::GetColorU32(muted), digit);
        }
        ImGui::Dummy(ImVec2(radius * 2, radius * 2));
        ImGui::SameLine(0, S(8));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(now ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : muted, "%s", names[i]);
    }
}
} // namespace ui

std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) out += static_cast<char>(std::tolower(c));
    return out;
}

struct ExternalSong {
    std::string source; // e.g. "mod 'KevinMacLeod'" or "the game soundtrack"
    fs::path folder;    // folder of the mod if from a mod, empty if from game
};

template<std::size_t N> void copy_text(std::array<char, N>& to, const std::string& from) {
    const auto n = std::min(from.size(), N - 1);
    std::copy_n(from.data(), n, to.data());
    to[n] = '\0';
}

// ---- Settings: the game folder and where ffmpeg is, in %APPDATA%\ReSkateMusicPacker (see platform.h) --
struct Settings {
    fs::path game, ffmpeg;
};
fs::path settings_file() {
    const auto base = platform::config_directory();
    return (base.empty() ? fs::path{} : base / L"ReSkateMusicPacker") / L"settings.json";
}
Settings load_settings() {
    Settings settings;
    try {
        std::ifstream in(settings_file(), std::ios::binary);
        if (!in) return settings;
        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
        settings.game = widen(root.value("game", ""));
        settings.ffmpeg = widen(root.value("ffmpeg", ""));
    } catch (...) {}
    return settings;
}
void save_settings(const Settings& settings) {
    auto root = Json::object();
    root["game"] = narrow(settings.game.wstring());
    root["ffmpeg"] = narrow(settings.ffmpeg.wstring());
    std::error_code ignored;
    fs::create_directories(settings_file().parent_path(), ignored);
    std::ofstream(settings_file(), std::ios::binary) << root.dump(2) << "\n";
}

// ---- Platform bits (platform.h, file_dialog.h) ----------------------------------------------------
bool game_folder(const fs::path& folder) { return !folder.empty() && fs::exists(dingosdk::resolve_case(folder / L"Skate.exe")); }

// "ffmpeg.exe" on Windows, "ffmpeg" elsewhere.
std::string program(const char* name) { return std::string(name) + platform::executable_suffix; }
const std::string ffmpeg_pair = program("ffmpeg") + " and " + program("ffprobe");

// ffmpeg and ffprobe on PATH, or in the folder the user pointed at (added to this process's PATH).
bool find_ffmpeg(const fs::path& extra) {
    if (!extra.empty() && fs::exists(extra / program("ffmpeg"))) platform::add_to_path(extra);
    return platform::on_path("ffmpeg") && platform::on_path("ffprobe");
}

// Asked every frame by the status bar, so the process list is walked at most every two seconds.
bool game_running() {
    static auto checked = std::chrono::steady_clock::time_point{};
    static bool running = false;
    const auto now = std::chrono::steady_clock::now();
    if (now - checked > std::chrono::seconds(2)) {
        running = platform::process_running("Skate.exe");
        checked = now;
    }
    return running;
}

#if defined(_WIN32)
constexpr const char* file_manager = "File Explorer";
#elif defined(__APPLE__)
constexpr const char* file_manager = "Finder";
#else
constexpr const char* file_manager = "your file manager";
#endif
const std::string executable_name = program("ReSkateMusicPacker");

// Where to get ffmpeg on this system, for the ffmpeg page and Settings.
const char* ffmpeg_install_hint() {
#if defined(_WIN32)
    return "Install ffmpeg (for example a Windows build linked from the official download page), then point at the "
           "folder that holds ffmpeg.exe.";
#elif defined(__APPLE__)
    return "Install it with Homebrew (brew install ffmpeg) or MacPorts, then press Check again, or point at the "
           "folder that holds ffmpeg.";
#else
    return "Install it with your package manager (for example sudo apt install ffmpeg, sudo dnf install ffmpeg or "
           "sudo pacman -S ffmpeg), then press Check again, or point at the folder that holds ffmpeg.";
#endif
}

// A dropped or chosen folder brings in its audio files, including subfolders, sorted by name.
std::vector<fs::path> expand(const std::vector<fs::path>& paths) {
    static const std::set<std::wstring> audio{
        L".mp3", L".flac", L".ogg", L".opus", L".wav", L".m4a", L".aac", L".wma",
        L".aiff", L".aif", L".webm", L".mka", L".mp4"
    };
    std::vector<fs::path> files;
    for (const auto& path : paths) {
        std::error_code error;
        if (!fs::is_directory(path, error)) { files.push_back(path); continue; }
        std::vector<fs::path> inside;
        for (const auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, error)) {
            if (error) break;
            std::error_code ec;
            if (!entry.is_regular_file(ec)) continue;
            auto extension = entry.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
            if (audio.contains(extension)) inside.push_back(entry.path());
        }
        std::sort(inside.begin(), inside.end());
        files.insert(files.end(), inside.begin(), inside.end());
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

// A folder name from the mod's name: letters, digits, '-' and '_' ("Kevin MacLeod" -> KevinMacLeod).
std::wstring folder_name(const std::string& name) {
    std::string out;
    for (unsigned char c : name) if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
    return widen(out.empty() ? "ReSkateMusic" : out);
}

// ---- The window's state --------------------------------------------------------------------------
struct Row {
    fs::path file;
    std::array<char, 256> artist{}, title{};
    std::array<char, 128> playlist{};
    double seconds{};
    std::vector<std::string> problems; // from scan()
    bool scanned{};
    fs::path artwork;
    bool has_embedded_artwork{};
};

struct ArtworkPreviewResult {
    std::uint64_t generation{};
    std::vector<unsigned char> pixels;
    unsigned width{}, height{};
    std::string error;
};

// A finished ffmpeg download: the install folder on success, or a message fit to show.
struct FfmpegInstallResult {
    bool ok{};
    fs::path folder;
    std::string error;
};

struct App {
    Settings settings;
    bool ffmpeg{};
    bool settings_open{};
    std::array<char, 128> name{}, playlist{};
    int bitrate = 2; // index into bitrates
    bool normalize = true;
    std::map<std::string, fs::path> playlist_artwork;
    std::set<std::string> generated_playlist_artwork;
    Renderer* renderer{};
    ImTextureID artwork_preview{};
    std::string artwork_preview_label, artwork_preview_error;
    std::uint64_t artwork_preview_generation{}; // UI-owned; workers capture a value, never read it
    bool artwork_preview_loading{};
    std::vector<Row> rows;
    fs::path output; // empty: Mods\<folder_name(name)>
    std::string status;
    bool status_error{};

    // One background job at a time (the packer shares a scratch folder): scanning added files or building.
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> busy{}, cancel{};
    std::string job;                                    // "Scanning" / "Building"
    float progress{};
    std::string progress_text;
    std::vector<std::pair<fs::path, music::SongInfo>> scanned;  // finished scans to apply
    std::optional<std::pair<bool, std::string>> finished;       // a build's result: ok, message
    std::optional<ArtworkPreviewResult> artwork_preview_ready; // guarded by mutex
    std::optional<FfmpegInstallResult> ffmpeg_install;          // a download's result, guarded by mutex

    std::vector<fs::path> dropped;
    std::mutex dropped_mutex;

    bool show_export_ts = false;
    bool show_artwork_modal = false;
    bool show_playlist_artwork_prompt = false;
    std::array<char, 64> ts_author{"Author"};
    std::array<char, 32> ts_version{"1.0.0"};
    std::array<char, 256> ts_description{};
    fs::path ts_icon;
    fs::path ts_output_folder;
    bool ts_open_explorer = true;
    bool ts_readme_credit = true;

    std::string active_playlist_filter;
    std::optional<std::string> select_playlist_tab;
    std::vector<std::string> custom_playlists;
    bool show_new_playlist_modal = false;
    std::array<char, 64> new_playlist_input{};
    int new_playlist_row_target = -1;

    enum class PendingAction { None, NewMod, OpenMod };
    PendingAction pending_action = PendingAction::None;
    bool show_confirm_discard_modal = false;

    std::map<std::string, ExternalSong> external_songs;
} *g_app;

void set_status(App& app, std::string text, bool error = false) { app.status = std::move(text); app.status_error = error; }
void clear_artwork_preview(App& app) {
    ++app.artwork_preview_generation; // Invalidate any result still being prepared.
    app.artwork_preview_loading = false;
    if (app.artwork_preview) app.renderer->release_texture(app.artwork_preview);
    app.artwork_preview = {};
    app.artwork_preview_label.clear();
    app.artwork_preview_error.clear();
}

fs::path output_folder(const App& app) {
    return !app.output.empty() ? app.output : app.settings.game / L"Mods" / folder_name(app.name.data());
}

// Scans other installed mods in Mods/ and the game's content cache for existing songs to warn on clash.
void refresh_external_songs(App& app) {
    app.external_songs.clear();
    if (!game_folder(app.settings.game)) return;

    std::error_code ec;
    const auto mods_folder = app.settings.game / L"Mods";
    if (fs::exists(mods_folder, ec) && fs::is_directory(mods_folder, ec)) {
        for (const auto& entry : fs::directory_iterator(mods_folder, ec)) {
            if (!entry.is_directory(ec)) continue;
            const auto mod_path = entry.path();
            const auto mod_name = narrow(mod_path.filename().wstring());

            const auto music_json = mod_path / L"reskate-music.json";
            if (fs::exists(music_json, ec)) {
                try {
                    std::ifstream in(music_json, std::ios::binary);
                    if (in) {
                        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
                        if (root.contains("playlists") && root["playlists"].is_array()) {
                            for (const auto& pl : root["playlists"]) {
                                if (pl.contains("songs") && pl["songs"].is_array()) {
                                    for (const auto& s : pl["songs"]) {
                                        if (s.is_string()) {
                                            app.external_songs.try_emplace(lower(s.get<std::string>()),
                                                ExternalSong{"mod '" + mod_name + "'", mod_path});
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {}
            }

            const auto proj_json = mod_path / L"reskate-music-project.json";
            if (fs::exists(proj_json, ec)) {
                try {
                    std::ifstream in(proj_json, std::ios::binary);
                    if (in) {
                        const auto root = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
                        if (root.contains("songs") && root["songs"].is_array()) {
                            for (const auto& s : root["songs"]) {
                                const auto artist = s.value("artist", "");
                                const auto title = s.value("title", "");
                                if (!artist.empty() && !title.empty()) {
                                    app.external_songs.try_emplace(lower(artist + " - " + title),
                                        ExternalSong{"mod '" + mod_name + "'", mod_path});
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }
    }

    try {
        for (const auto& cache_dir : platform::reskate_cache_directories(app.settings.game)) {
            if (fs::exists(cache_dir, ec) && fs::is_directory(cache_dir, ec)) {
                for (const auto& entry : fs::recursive_directory_iterator(cache_dir, fs::directory_options::skip_permission_denied, ec)) {
                    if (entry.is_regular_file(ec) && entry.path().extension() == L".cache") {
                        std::ifstream in(entry.path(), std::ios::binary);
                        if (!in) continue;
                        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        for (std::size_t i = 0; i + 4 < buf.size(); ++i) {
                            if (buf[i] == 0x12) {
                                std::size_t len = buf[i + 1];
                                std::size_t header_len = 2;
                                if (len & 0x80) {
                                    len = (len & 0x7F) | ((buf[i + 2] & 0x7F) << 7);
                                    header_len = 3;
                                }
                                if (len >= 5 && len < 200 && i + header_len + len <= buf.size()) {
                                    std::string s(reinterpret_cast<const char*>(&buf[i + header_len]), len);
                                    if (s.find(" - ") != std::string::npos && s.find('\0') == std::string::npos) {
                                        bool printable = true;
                                        for (char c : s) {
                                            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
                                                printable = false;
                                                break;
                                            }
                                        }
                                        if (printable) {
                                            app.external_songs.try_emplace(lower(s), ExternalSong{"the game soundtrack", {}});
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {}
}

void start(App& app, std::string job, std::function<void()> work) {
    if (app.worker.joinable()) app.worker.join();
    app.busy = true;
    app.cancel = false;
    app.job = std::move(job);
    app.progress = 0;
    app.progress_text.clear();
    app.worker = std::thread([&app, work = std::move(work)] {
        work();
        app.busy = false;
    });
}

void request_artwork_preview(App& app, std::string label, fs::path source, bool generated,
    std::vector<std::pair<fs::path, fs::path>> tracks = {}) {
    if (app.busy) return;
    clear_artwork_preview(app);
    app.artwork_preview_label = label;
    app.artwork_preview_loading = true;
    const auto generation = app.artwork_preview_generation;
    // Resolve embedded art, run FFmpeg and decode pixels off the UI thread. Inputs are
    // snapshots; row edits and renderer resources are never accessed by this worker.
    start(app, "Loading artwork", [&app, label = std::move(label), source = std::move(source), generated,
        tracks = std::move(tracks), generation]() mutable {
        ArtworkPreviewResult result;
        result.generation = generation;
#ifdef _WIN32
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Com { HRESULT status; ~Com() { if (SUCCEEDED(status)) CoUninitialize(); } } cleanup{com};
#endif
        try {
#ifdef _WIN32
            if (FAILED(com)) throw std::runtime_error("Could not initialise artwork decoding.");
#endif
            if (source.empty() && !generated)
                for (const auto& [file, artwork] : tracks) {
                    if (app.cancel) throw music::Cancelled();
                    source = artwork.empty() ? music::embedded_artwork(file) : artwork;
                    if (!source.empty()) break;
                }
            if (app.cancel) throw music::Cancelled();
            if (!generated && source.empty()) throw std::runtime_error("No artwork available for this cover.");
            const auto png = generated ? music::playlist_artwork_png(label) : music::image_artwork_png(source);
            if (app.cancel) throw music::Cancelled();
            std::vector<unsigned char> bytes(png.size());
            std::memcpy(bytes.data(), png.data(), png.size());
            if (!dingosdk::launcher_gui::detail::decode_image(bytes, ImVec2(512, 512), result.pixels, result.width, result.height))
                throw std::runtime_error("Could not preview the cover image.");
        } catch (const std::exception& error) { result.error = error.what(); }
        std::lock_guard lock(app.mutex);
        app.artwork_preview_ready = std::move(result);
    });
}

void apply_artwork_preview(App& app) {
    std::optional<ArtworkPreviewResult> ready;
    {
        std::lock_guard lock(app.mutex);
        ready = std::move(app.artwork_preview_ready);
        app.artwork_preview_ready.reset();
    }
    if (!ready || ready->generation != app.artwork_preview_generation) return;
    app.artwork_preview_loading = false;
    app.artwork_preview_error = std::move(ready->error);
    if (!app.artwork_preview_error.empty()) return;
    // Direct3D resources remain owned by the UI thread.
    try {
        const auto texture = app.renderer->upload_texture(ready->pixels, ready->width, ready->height);
        if (!texture) app.artwork_preview_error = "Could not display the cover preview.";
        else app.artwork_preview = texture;
    } catch (const std::exception& error) { app.artwork_preview_error = error.what(); }
}

std::vector<std::pair<fs::path, fs::path>> playlist_tracks(const App& app, const std::string& playlist_name) {
    std::vector<std::pair<fs::path, fs::path>> tracks;
    for (const auto& row : app.rows) {
        const auto pl = row.playlist[0] ? row.playlist.data() : app.playlist.data();
        if (playlist_name == pl) tracks.emplace_back(row.file, row.artwork);
    }
    return tracks;
}

void request_playlist_preview(App& app, const std::string& playlist_name) {
    auto it = app.playlist_artwork.find(playlist_name);
    fs::path source = (it != app.playlist_artwork.end()) ? it->second : fs::path{};
    const bool gen = app.generated_playlist_artwork.contains(playlist_name);
    auto tracks = playlist_tracks(app, playlist_name);
    request_artwork_preview(app, playlist_name, std::move(source), gen, std::move(tracks));
}

// Downloads and installs the pinned ffmpeg build on the one-job worker. Never runs by itself: the
// button that calls this shows the source, size and licence first.
void install_ffmpeg(App& app) {
    if (app.busy) return;
    start(app, "Downloading ffmpeg", [&app] {
        FfmpegInstallResult result;
        try {
            const fs::path directory = music::default_install_dir();
            music::ensure_ffmpeg(directory, music::ffmpeg_url(), music::ffmpeg_sha256(),
                [&app](const music::DownloadProgress& step) {
                    std::lock_guard lock(app.mutex);
                    constexpr double megabytes = 1024.0 * 1024.0;
                    const auto received = static_cast<int>(static_cast<double>(step.received) / megabytes);
                    if (step.total) {
                        app.progress = static_cast<float>(static_cast<double>(step.received) / static_cast<double>(step.total));
                        app.progress_text = std::to_string(received) + " / " +
                                            std::to_string(static_cast<int>(static_cast<double>(step.total) / megabytes)) + " MB";
                    } else app.progress_text = std::to_string(received) + " MB";
                }, &app.cancel);
            result.ok = true;
            result.folder = directory;
        } catch (const music::Cancelled&) {
            result.error = "Cancelled; ffmpeg was not installed.";
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        std::lock_guard lock(app.mutex);
        app.ffmpeg_install = std::move(result);
    });
}

void add_files(App& app, const std::vector<fs::path>& paths, const std::string& target_playlist = "") {
    auto files = expand(paths);
    std::erase_if(files, [&](const fs::path& file) {
        return std::any_of(app.rows.begin(), app.rows.end(), [&](const Row& row) { return row.file == file; });
    });
    if (files.empty() || app.busy) return;
    const std::string pl = !target_playlist.empty() ? target_playlist : app.active_playlist_filter;
    for (const auto& file : files) {
        Row row{file};
        if (!pl.empty() && pl != (app.playlist[0] ? app.playlist.data() : "Default")) {
            copy_text(row.playlist, pl);
        }
        app.rows.push_back(std::move(row));
    }
    start(app, "Scanning", [&app, files] {
        for (std::size_t i = 0; i < files.size() && !app.cancel; ++i) {
            const std::vector<fs::path> one{files[i]};
            auto info = music::scan(one)[0];
            std::lock_guard lock(app.mutex);
            app.scanned.emplace_back(files[i], std::move(info));
            app.progress = static_cast<float>(i + 1) / static_cast<float>(files.size());
            app.progress_text = narrow(files[i].filename().wstring());
        }
    });
}

void reset_mod(App& app) {
    app.rows.clear();
    app.output.clear();
    app.active_playlist_filter.clear();
    app.custom_playlists.clear();
    app.select_playlist_tab.reset();
    app.name.fill(0);
    app.playlist.fill(0);
    app.normalize = true;
    app.playlist_artwork.clear();
    app.generated_playlist_artwork.clear();
    clear_artwork_preview(app);
    set_status(app, "");
}

bool has_unsaved_changes(const App& app) {
    return !app.rows.empty() || app.name[0] != '\0' || app.playlist[0] != '\0' ||
           !app.custom_playlists.empty() || !app.playlist_artwork.empty() ||
           !app.generated_playlist_artwork.empty();
}

void open_mod(App& app, const fs::path& folder) {
    try {
        const auto project = music::load_project(folder);
        clear_artwork_preview(app);
        app.rows.clear();
        app.active_playlist_filter.clear();
        app.custom_playlists.clear();
        app.select_playlist_tab.reset();
        copy_text(app.name, project.name);
        copy_text(app.playlist, project.playlist);
        app.bitrate = 2;
        for (int i = 0; i < 5; ++i) if (std::stoi(bitrates[i]) == project.bitrate) app.bitrate = i;
        app.normalize = project.normalize;
        app.playlist_artwork = project.playlist_artwork;
        app.generated_playlist_artwork = project.generated_playlist_artwork;
        app.output = folder;
        refresh_external_songs(app);
        std::vector<fs::path> files;
        for (const auto& song : project.songs) {
            Row row{song.file};
            copy_text(row.artist, song.artist);
            copy_text(row.title, song.title);
            copy_text(row.playlist, song.playlist);
            row.artwork = song.artwork;
            app.rows.push_back(row);
            files.push_back(song.file);
        }
        // Scan for lengths and problems; the project's artist/title stay (see apply_scans).
        start(app, "Scanning", [&app, files] {
            for (std::size_t i = 0; i < files.size() && !app.cancel; ++i) {
                const std::vector<fs::path> one{files[i]};
                auto info = music::scan(one)[0];
                std::lock_guard lock(app.mutex);
                app.scanned.emplace_back(files[i], std::move(info));
                app.progress = static_cast<float>(i + 1) / static_cast<float>(files.size());
            }
        });
        set_status(app, "Opened " + narrow(folder.filename().wstring()) + ".");
    } catch (const std::exception& error) {
        set_status(app, error.what(), true);
    }
}

void apply_scans(App& app) {
    std::lock_guard lock(app.mutex);
    for (auto& [file, info] : app.scanned)
        for (auto& row : app.rows)
            if (row.file == file && !row.scanned) {
                row.scanned = true;
                row.seconds = info.seconds;
                row.has_embedded_artwork = info.has_embedded_artwork;
                // Problems about the tags fall away once artist and title are filled in by hand.
                std::erase_if(info.problems, [](const std::string& p) { return p.find("tag") != std::string::npos; });
                row.problems = info.problems;
                if (!row.artist[0]) copy_text(row.artist, info.artist);
                if (!row.title[0]) copy_text(row.title, info.title);
            }
    app.scanned.clear();
}

// Problems that block building, per row (empty: fine) and for the whole mod.
std::vector<std::string> row_problems(const App& app, std::size_t index) {
    const auto& row = app.rows[index];
    auto problems = row.problems;
    if (!music::usable_name(row.artist.data())) problems.push_back("needs an artist");
    if (!music::usable_name(row.title.data())) problems.push_back("needs a title");
    if (row.playlist[0] && !music::usable_name(row.playlist.data())) problems.push_back("invalid playlist name");
    const auto id = std::string(row.artist.data()) + " - " + row.title.data();
    for (std::size_t other = 0; other < app.rows.size(); ++other)
        if (other != index && std::string(app.rows[other].artist.data()) + " - " + app.rows[other].title.data() == id) {
            problems.push_back("same artist and title as song " + std::to_string(other + 1));
            break;
        }
    if (row.artist[0] && row.title[0]) {
        if (auto it = app.external_songs.find(lower(id)); it != app.external_songs.end()) {
            std::error_code ec;
            const auto current_mod = output_folder(app);
            if (it->second.folder.empty() || !fs::equivalent(it->second.folder, current_mod, ec)) {
                problems.push_back("already in " + it->second.source);
            }
        }
    }
    return problems;
}

void build(App& app) {
    music::PackOptions options;
    options.game = app.settings.game;
    options.output = output_folder(app);
    options.name = app.name.data();
    options.playlist = app.playlist.data();
    options.bitrate = std::stoi(bitrates[app.bitrate]);
    options.normalize = app.normalize;
    options.playlist_artwork = app.playlist_artwork;
    options.generated_playlist_artwork = app.generated_playlist_artwork;
    std::vector<music::SongInfo> songs;
    for (const auto& row : app.rows) {
        music::SongInfo song{row.file, row.artist.data(), row.title.data(), row.playlist.data()};
        song.artwork = row.artwork;
        songs.push_back(std::move(song));
    }
    start(app, "Building", [&app, options, songs] {
        std::pair<bool, std::string> result;
        try {
            const auto packed = music::pack(options, songs, [&app](const music::Progress& step) {
                std::lock_guard lock(app.mutex);
                const auto done = static_cast<float>(step.song) + (std::string(step.stage) == "encoded" ? 1.f : 0.f);
                app.progress = std::min(done / static_cast<float>(step.count + 1), 1.f);
                app.progress_text = step.song < step.count ? std::string(step.stage) + " song " + std::to_string(step.song + 1) + " of " +
                                                                 std::to_string(step.count)
                                                           : std::string(step.stage) + " the mod";
            }, &app.cancel);
            result = {true, "Built " + std::to_string(packed.songs) + " song(s) into " + narrow(packed.output.wstring()) +
                                ". Start the game through the ReSkate launcher to hear them."};
        } catch (const music::Cancelled&) {
            result = {false, "Cancelled; nothing was written."};
        } catch (const std::exception& error) {
            result = {false, error.what()};
        }
        std::lock_guard lock(app.mutex);
        app.finished = result;
    });
}

// ---- Drawing -------------------------------------------------------------------------------------
void apply_theme() {
    auto& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(S(16.0f), S(14.0f));
    style.FramePadding      = ImVec2(S(9.0f),  S(6.0f));
    style.CellPadding       = ImVec2(S(8.0f),  S(6.0f));
    style.ItemSpacing       = ImVec2(S(8.0f),  S(8.0f));
    style.ItemInnerSpacing  = ImVec2(S(6.0f),  S(6.0f));
    style.ScrollbarSize     = S(13.0f);
    style.GrabMinSize       = S(10.0f);

    style.WindowRounding    = S(0.0f);
    style.ChildRounding     = S(8.0f);
    style.FrameRounding     = S(6.0f);
    style.PopupRounding     = S(8.0f);
    style.ScrollbarRounding = S(6.0f);
    style.GrabRounding      = S(4.0f);
    style.TabRounding       = S(6.0f);

    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;


    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.13f, 0.14f, 0.17f, 0.98f);
    colors[ImGuiCol_Border]               = ImVec4(0.22f, 0.24f, 0.28f, 0.70f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.54f, 0.58f, 1.00f);

    colors[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.21f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.29f, 0.36f, 1.00f);

    colors[ImGuiCol_TitleBg]              = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.09f, 0.10f, 0.12f, 0.75f);
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);

    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.11f, 0.13f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.26f, 0.29f, 0.35f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.38f, 0.46f, 0.90f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.42f, 0.47f, 0.56f, 1.00f);

    colors[ImGuiCol_CheckMark]            = ImVec4(0.22f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.22f, 0.58f, 0.95f, 0.90f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.30f, 0.68f, 1.00f, 1.00f);

    colors[ImGuiCol_Button]               = ImVec4(0.19f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.26f, 0.31f, 0.39f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.16f, 0.20f, 0.25f, 1.00f);

    colors[ImGuiCol_Header]               = ImVec4(0.18f, 0.21f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.28f, 0.34f, 0.42f, 1.00f);

    colors[ImGuiCol_Separator]            = ImVec4(0.22f, 0.24f, 0.28f, 0.80f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.28f, 0.32f, 0.38f, 0.90f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(0.35f, 0.42f, 0.52f, 1.00f);

    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.22f, 0.58f, 0.95f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.22f, 0.58f, 0.95f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.22f, 0.58f, 0.95f, 0.95f);

    colors[ImGuiCol_TableHeaderBg]        = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]    = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_TableBorderLight]     = ImVec4(0.18f, 0.20f, 0.24f, 0.70f);
    colors[ImGuiCol_TableRowBg]           = ImVec4(0.12f, 0.13f, 0.16f, 0.70f);
    colors[ImGuiCol_TableRowBgAlt]        = ImVec4(0.14f, 0.15f, 0.18f, 0.70f);

    colors[ImGuiCol_Tab]                  = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.24f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_TabActive]            = ImVec4(0.19f, 0.23f, 0.29f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.16f, 0.18f, 0.23f, 1.00f);

    colors[ImGuiCol_PlotHistogram]        = ImVec4(0.18f, 0.52f, 0.92f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 0.60f, 1.00f, 1.00f);

    colors[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.05f, 0.05f, 0.07f, 0.65f);
}

// Where the game usually is on this system, for the setup page.
const char* game_folder_hint() {
#if defined(_WIN32)
    return "It's the folder with Skate.exe and ReSkateLauncher.exe, for example F:\\Games\\ReSkate-1.0.0.";
#elif defined(__APPLE__)
    return "It's the folder with Skate.exe and ReSkateLauncher.exe inside your CrossOver or Whisky bottle's drive_c.";
#else
    return "It's the folder with Skate.exe and ReSkateLauncher.exe: under Steam/Proton usually "
           "~/.local/share/Steam/steamapps/common/<game>, or inside a Wine prefix's drive_c.";
#endif
}

void card_status(const App& app) {
    if (app.status.empty()) return;
    ImGui::Spacing();
    ui::status_text(app.status, app.status_error);
}

void setup_page(App& app, NativeWindow window) {
    if (ui::begin_card("setup", ImVec2(S(640), S(330)))) {
        ui::steps(0);
        ImGui::Dummy(ImVec2(0, S(14)));
        ui::heading("Where is skate. installed?");
        ImGui::Spacing();
        ImGui::TextWrapped("The packer reads the game's own songs to build yours, so it needs the game folder first.");
        ImGui::PushStyleColor(ImGuiCol_Text, ui::muted);
        ImGui::TextWrapped("%s", game_folder_hint());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(14)));
        if (ui::primary_button("Choose the game folder...", ImVec2(S(240), S(38)))) {
            const auto folders = pick(window, true);
            if (!folders.empty()) {
                if (game_folder(folders[0])) {
                    app.settings.game = folders[0];
                    save_settings(app.settings);
                    refresh_external_songs(app);
                    set_status(app, "");
                } else set_status(app, "That folder has no Skate.exe.", true);
            }
        }
        card_status(app);
    }
    ui::end_card();
}

// The one-click installer, shared by first-run setup and the Settings dialog. Downloading only ever
// starts from a click here; the source, size and licence sit under the button.
void ffmpeg_download_controls(App& app, float width) {
    if (app.busy && app.job == "Downloading ffmpeg") {
        std::lock_guard lock(app.mutex);
        const std::string label = app.progress_text.empty() ? "Starting..." : app.progress_text;
        ImGui::ProgressBar(app.progress, ImVec2(width, S(26)), label.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1, S(26)))) app.cancel = true;
        return;
    }
    ImGui::BeginDisabled(app.busy);
    const bool clicked = ImGui::Button("Download ffmpeg automatically", ImVec2(width, S(32)));
    ImGui::EndDisabled();
    ImGui::TextDisabled("Static LGPL build from BtbN/FFmpeg-Builds on GitHub (~163 MB). "
                        "Downloaded only when you click, then checked against a pinned SHA-256.");
    if (clicked) install_ffmpeg(app);
}

void ffmpeg_page(App& app, NativeWindow window) {
    if (ui::begin_card("ffmpeg", ImVec2(S(660), S(380)))) {
        ui::steps(1);
        ImGui::Dummy(ImVec2(0, S(14)));
        ui::heading("One more thing: ffmpeg");
        ImGui::Spacing();
        ImGui::TextWrapped("The packer uses ffmpeg to read and encode songs, and cannot find %s. %s",
                           ffmpeg_pair.c_str(), ffmpeg_install_hint());
        ImGui::Dummy(ImVec2(0, S(12)));
        if (music::ffmpeg_download_supported()) {
            ffmpeg_download_controls(app, S(300));
            ImGui::Spacing();
            ImGui::TextDisabled("- or install it yourself -");
            ImGui::Spacing();
        } else {
            if (ui::primary_button("Check again", ImVec2(S(160), S(34)))) {
                app.ffmpeg = find_ffmpeg(app.settings.ffmpeg);
                set_status(app, app.ffmpeg ? "" : "Still no " + ffmpeg_pair + " on PATH.", !app.ffmpeg);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Open the ffmpeg download page", ImVec2(S(240), S(34))))
            platform::open_url("https://ffmpeg.org/download.html");
        ImGui::SameLine();
        if (ImGui::Button("Locate ffmpeg...", ImVec2(S(160), S(34)))) {
            const auto folders = pick(window, true);
            if (!folders.empty()) {
                auto folder = folders[0];
                if (!fs::exists(folder / program("ffmpeg")) && fs::exists(folder / L"bin" / program("ffmpeg"))) folder /= L"bin";
                if (find_ffmpeg(folder)) {
                    app.settings.ffmpeg = folder;
                    save_settings(app.settings);
                    app.ffmpeg = true;
                    set_status(app, "");
                } else set_status(app, "That folder has no " + ffmpeg_pair + ".", true);
            }
        }
        card_status(app);
    }
    ui::end_card();
}

// Reconfigures the game folder and ffmpeg after first-run setup; reachable from any page.
void settings_modal(App& app, NativeWindow window) {
    if (app.settings_open) { ImGui::OpenPopup("Settings"); app.settings_open = false; }
    if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(540));

    ImGui::TextColored(ImVec4(0.22f, 0.65f, 1.0f, 1.0f), "Game folder");
    ImGui::TextWrapped("%s", app.settings.game.empty() ? "Not set." : narrow(app.settings.game.wstring()).c_str());
    if (ImGui::Button("Choose game folder...", ImVec2(S(190), S(28)))) {
        const auto folders = pick(window, true);
        if (!folders.empty()) {
            if (game_folder(folders[0])) {
                app.settings.game = folders[0];
                save_settings(app.settings);
                refresh_external_songs(app);
                set_status(app, "");
            } else set_status(app, "That folder has no Skate.exe.", true);
        }
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.22f, 0.65f, 1.0f, 1.0f), "ffmpeg");
    ImGui::TextWrapped("%s", app.ffmpeg ? (ffmpeg_pair + " found.").c_str()
                                        : ("Not found - the packer needs " + ffmpeg_pair + ". " + ffmpeg_install_hint()).c_str());
    if (!app.ffmpeg && music::ffmpeg_download_supported()) {
        ffmpeg_download_controls(app, S(250));
        ImGui::Spacing();
    }
    if (ImGui::Button("Open the ffmpeg download page", ImVec2(S(230), S(28))))
        platform::open_url("https://ffmpeg.org/download.html");
    ImGui::SameLine();
    if (ImGui::Button("Locate ffmpeg...", ImVec2(S(150), S(28)))) {
        const auto folders = pick(window, true);
        if (!folders.empty()) {
            auto folder = folders[0];
            if (!fs::exists(folder / program("ffmpeg")) && fs::exists(folder / L"bin" / program("ffmpeg"))) folder /= L"bin";
            if (find_ffmpeg(folder)) {
                app.settings.ffmpeg = folder;
                save_settings(app.settings);
                app.ffmpeg = true;
                set_status(app, "");
            } else set_status(app, "That folder has no " + ffmpeg_pair + ".", true);
        }
    }

    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(S(120), S(28)))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void songs_page(App& app, NativeWindow window) {
    const bool busy = app.busy;

    const std::string defaultPlaylistName = app.playlist[0] ? app.playlist.data() : "Default";
    std::vector<std::string> unique_playlists;
    std::map<std::string, std::pair<std::size_t, double>> playlist_stats;
    unique_playlists.push_back(defaultPlaylistName);
    playlist_stats[defaultPlaylistName] = {0, 0.0};

    for (const auto& cpl : app.custom_playlists) {
        if (std::find(unique_playlists.begin(), unique_playlists.end(), cpl) == unique_playlists.end()) {
            unique_playlists.push_back(cpl);
            playlist_stats[cpl] = {0, 0.0};
        }
    }

    for (const auto& r : app.rows) {
        const std::string pl = r.playlist[0] ? r.playlist.data() : defaultPlaylistName;
        if (std::find(unique_playlists.begin(), unique_playlists.end(), pl) == unique_playlists.end()) {
            unique_playlists.push_back(pl);
        }
        auto& stats = playlist_stats[pl];
        stats.first++;
        if (r.scanned) stats.second += r.seconds;
    }

    if (!app.active_playlist_filter.empty() &&
        std::find(unique_playlists.begin(), unique_playlists.end(), app.active_playlist_filter) == unique_playlists.end()) {
        app.active_playlist_filter.clear();
    }

    // Per-song problems and what stops a build, for the Build button and the status bar.
    std::vector<std::vector<std::string>> problems(app.rows.size());
    std::size_t blocked = 0;
    for (std::size_t i = 0; i < app.rows.size(); ++i) {
        problems[i] = row_problems(app, i);
        if (!problems[i].empty()) ++blocked;
    }
    std::string block;
    if (app.rows.empty()) block = "Add some songs to build a mod.";
    else if (!music::usable_name(app.name.data())) block = "Give the mod a name.";
    else if (!music::usable_name(app.playlist.data())) block = "Give the default playlist a name.";
    else if (blocked) block = std::to_string(blocked) + (blocked == 1 ? " song needs" : " songs need") + " fixing (see Problems).";
    else if (std::any_of(app.rows.begin(), app.rows.end(), [](const Row& r) { return !r.scanned; })) block = "Reading the songs...";

    const auto assign_playlist = [&](Row& row, const std::string& name) {
        if (name == defaultPlaylistName) row.playlist.fill(0);
        else copy_text(row.playlist, name);
    };

    if (app.show_artwork_modal) {
        ImGui::OpenPopup("Track and playlist artwork");
        app.show_artwork_modal = false;
    }


    if (ImGui::BeginPopupModal("Track and playlist artwork", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(700));
        ImGui::TextWrapped("Track covers use embedded album art unless you choose an image. Playlists use their first track cover, or you can choose an image or generate a text cover.");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(busy);
        const auto preview = [&](const std::string& label, const fs::path& source, bool generated) {
            request_artwork_preview(app, label, source, generated);
        };
        const auto choose = [&](const std::string& label, fs::path& image) {
            bool selected = false;
            ImGui::PushID(label.c_str());
            ImGui::TextWrapped("%s", label.c_str());
            ImGui::TextDisabled("%s", image.empty() ? "Automatic cover" : narrow(image.filename().wstring()).c_str());
            if (!image.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(image.wstring()).c_str());
            if (ImGui::Button("Choose image...")) {
                const auto files = pick(window, false, true);
                if (!files.empty()) { image = files[0]; selected = true; preview(label, image, false); }
            }
            ImGui::SameLine();
            if (ImGui::Button("Use automatic")) { image.clear(); clear_artwork_preview(app); }
            ImGui::PopID();
            return selected;
        };
        if (ImGui::BeginChild("covers", ImVec2(S(480), S(360)))) {
            ImGui::TextUnformatted("Playlists");
            std::set<std::string> names;
            if (app.playlist[0]) names.insert(app.playlist.data());
            for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
            ImGui::PushID("playlists");
            for (const auto& name : names) {
                if (choose(name, app.playlist_artwork[name])) app.generated_playlist_artwork.erase(name);
                ImGui::PushID(name.c_str());
                bool generated = app.generated_playlist_artwork.contains(name);
                if (ImGui::Checkbox("Generate text cover", &generated)) {
                    if (generated) {
                        app.generated_playlist_artwork.insert(name);
                        app.playlist_artwork[name].clear();
                        request_playlist_preview(app, name);
                    } else {
                        app.generated_playlist_artwork.erase(name);
                        request_playlist_preview(app, name);
                    }
                }
                if (ImGui::Button("Preview")) {
                    request_playlist_preview(app, name);
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::PopID();
            ImGui::TextUnformatted("Tracks");
            for (std::size_t i = 0; i < app.rows.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                auto& row = app.rows[i];
                choose(std::string(row.artist.data()) + " - " + row.title.data(), row.artwork);
                if (row.artwork.empty()) ImGui::TextDisabled("%s", row.has_embedded_artwork ? "Embedded album art detected" : "No embedded album art detected");
                if (ImGui::Button("Preview"))
                    request_artwork_preview(app, std::string(row.artist.data()) + " - " + row.title.data(),
                        row.artwork, false, {{row.file, {}}});
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("cover-preview", ImVec2(S(220), S(360)))) {
            ImGui::TextWrapped("%s", app.artwork_preview_label.empty() ? "Cover preview" : app.artwork_preview_label.c_str());
            if (app.artwork_preview_loading) ImGui::TextWrapped("Loading artwork...");
            else if (app.artwork_preview) ImGui::Image(app.artwork_preview, ImVec2(S(200), S(200)));
            else ImGui::TextWrapped("Choose a cover or click Preview.");
            if (!app.artwork_preview_error.empty()) ImGui::TextWrapped("%s", app.artwork_preview_error.c_str());
        }
        ImGui::EndChild();
        ImGui::EndDisabled();
        if (ImGui::Button("Done", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (app.show_new_playlist_modal) {
        ImGui::OpenPopup("New Playlist");
        app.show_new_playlist_modal = false;
    }
    if (ImGui::BeginPopupModal("New Playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Enter a name for the new playlist:");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(S(260));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter_pressed = ImGui::InputText("##new_pl_name", app.new_playlist_input.data(), app.new_playlist_input.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        const bool has_name = music::usable_name(app.new_playlist_input.data());
        ImGui::BeginDisabled(!has_name);
        if (ImGui::Button("Create", ImVec2(S(100), 0)) || (enter_pressed && has_name)) {
            const std::string new_pl = app.new_playlist_input.data();
            if (std::find(app.custom_playlists.begin(), app.custom_playlists.end(), new_pl) == app.custom_playlists.end()) {
                app.custom_playlists.push_back(new_pl);
            }
            if (app.new_playlist_row_target >= 0 && app.new_playlist_row_target < static_cast<int>(app.rows.size())) {
                copy_text(app.rows[app.new_playlist_row_target].playlist, new_pl);
                app.new_playlist_row_target = -1;
            }
            app.active_playlist_filter = new_pl;
            app.select_playlist_tab = new_pl;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            app.new_playlist_row_target = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (app.show_confirm_discard_modal) {
        ImGui::OpenPopup("Discard Changes?");
        app.show_confirm_discard_modal = false;
    }
    if (ImGui::BeginPopupModal("Discard Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(380));
        ImGui::TextWrapped("You have songs or metadata loaded. Starting a new mod or opening another will discard your current progress.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Discard and Continue", ImVec2(S(160), 0))) {
            const auto action = app.pending_action;
            app.pending_action = App::PendingAction::None;
            ImGui::CloseCurrentPopup();
            if (action == App::PendingAction::NewMod) {
                reset_mod(app);
            } else if (action == App::PendingAction::OpenMod) {
                const auto folders = pick(window, true);
                if (!folders.empty()) open_mod(app, folders[0]);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            app.pending_action = App::PendingAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Sidebar: the mod, its playlists, the cover, audio settings and Build ----------------------
    const auto& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::panel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14.0f), S(12.0f)));
    if (ImGui::BeginChild("sidebar", ImVec2(S(290.0f), 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const float full = ImGui::GetContentRegionAvail().x;
        const float half = (full - style.ItemSpacing.x) * 0.5f;

        ui::section("MOD");
        const float settings_w = ImGui::CalcTextSize("Settings").x + style.FramePadding.x * 2;
        ImGui::SameLine(full - settings_w + style.WindowPadding.x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1));
        if (ui::ghost_button("Settings", ImVec2(settings_w, ImGui::GetTextLineHeight() + S(2)))) app.settings_open = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Game folder and ffmpeg");
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(busy);
        if (ImGui::InputTextWithHint("##name", "Mod name", app.name.data(), app.name.size()) && !app.output.empty() &&
            app.output.parent_path() == app.settings.game / L"Mods")
            app.output.clear(); // a renamed new mod goes to its new folder; an opened one stays where it is
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mod name (used for the mod folder in Mods/)");
        {
            const auto where = narrow(output_folder(app).wstring());
            ImGui::PushStyleColor(ImGuiCol_Text, ui::muted);
            ImGui::TextUnformatted(ui::fit_middle("Builds into " + where, full).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", where.c_str());
        }
        if (ImGui::Button("New Mod", ImVec2(half, 0))) {
            if (has_unsaved_changes(app)) {
                app.pending_action = App::PendingAction::NewMod;
                app.show_confirm_discard_modal = true;
            } else {
                reset_mod(app);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Mod...", ImVec2(half, 0))) {
            if (has_unsaved_changes(app)) {
                app.pending_action = App::PendingAction::OpenMod;
                app.show_confirm_discard_modal = true;
            } else {
                const auto folders = pick(window, true);
                if (!folders.empty()) open_mod(app, folders[0]);
            }
        }
        ImGui::EndDisabled();

        // Playlists, cover and audio scroll when the window is short; Build stays pinned below them.
        const float bottom = S(42) + ImGui::GetFrameHeight() + style.ItemSpacing.y * 2;
        ImGui::BeginChild("sidebar_scroll", ImVec2(0, std::max(S(80), ImGui::GetContentRegionAvail().y - bottom)), 0,
                          ImGuiWindowFlags_NoBackground);

        // Playlists: a list that filters the songs; dropping a song (dragged by its number) moves it there.
        ui::section("PLAYLISTS");
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1));
        ImGui::BeginDisabled(busy);
        if (ui::ghost_button("+##new_playlist", ImVec2(ImGui::GetFrameHeight(), ImGui::GetTextLineHeight() + S(2)))) {
            app.show_new_playlist_modal = true;
            app.new_playlist_input.fill(0);
            app.new_playlist_row_target = -1;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new playlist");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##playlist", "Default playlist name", app.playlist.data(), app.playlist.size());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default playlist name in skate. Songs use this unless given their own playlist.");

        const float item_h = ImGui::GetFrameHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        {
            const auto entry = [&](const std::string& id, const std::string& label, std::size_t count, double seconds,
                                   bool selected, const char* tag) {
                const ImVec2 at = ImGui::GetCursorPos();
                const bool clicked = ImGui::Selectable(("##" + id).c_str(), selected, ImGuiSelectableFlags_AllowOverlap,
                                                       ImVec2(0, item_h));
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 after = ImGui::GetCursorPos();
                const std::string right = std::to_string(count);
                const float right_w = ImGui::CalcTextSize(right.c_str()).x;
                const float tag_w = tag ? ImGui::CalcTextSize(tag).x + S(8) : 0.0f;
                const float width = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPos(ImVec2(at.x + S(8), at.y + (item_h - ImGui::GetTextLineHeight()) * 0.5f));
                ImGui::TextUnformatted(ui::fit_middle(label, width - right_w - tag_w - S(24)).c_str());
                if (tag) {
                    ImGui::SameLine(0, S(8));
                    ImGui::TextColored(ui::muted, "%s", tag);
                }
                ImGui::SameLine(width - right_w - S(6));
                ImGui::TextColored(ui::muted, "%s", right.c_str());
                ImGui::SetCursorPos(after);
                if (hovered && count) ImGui::SetTooltip("%zu song%s, %s", count, count == 1 ? "" : "s", ui::duration(seconds).c_str());
                return clicked;
            };
            double all_seconds = 0;
            for (const auto& r : app.rows) all_seconds += r.scanned ? r.seconds : 0;
            if (entry("all", "All songs", app.rows.size(), all_seconds, app.active_playlist_filter.empty(), nullptr))
                app.active_playlist_filter.clear();
            for (const auto& pl : unique_playlists) {
                ImGui::PushID(pl.c_str());
                const auto& stats = playlist_stats[pl];
                const bool unnamed = pl == defaultPlaylistName && !app.playlist[0];
                if (entry("pl", unnamed ? "Unnamed playlist" : pl, stats.first, stats.second, app.active_playlist_filter == pl,
                          pl == defaultPlaylistName ? "default" : nullptr))
                    app.active_playlist_filter = pl;
                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("RSMP_ROW")) {
                        const auto index = *static_cast<const std::size_t*>(payload->Data);
                        if (index < app.rows.size()) assign_playlist(app.rows[index], pl);
                    }
                    ImGui::EndDragDropTarget();
                }
                if (pl != defaultPlaylistName && stats.first == 0 && ImGui::BeginPopupContextItem("playlist_menu")) {
                    if (ImGui::MenuItem("Remove empty playlist")) {
                        std::erase(app.custom_playlists, pl);
                        if (app.active_playlist_filter == pl) app.active_playlist_filter.clear();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopStyleVar();

        // The cover of the selected playlist (the default one under "All songs"), previewed live.
        const std::string cover_pl = app.active_playlist_filter.empty() ? defaultPlaylistName : app.active_playlist_filter;
        const bool cover_named = music::usable_name(cover_pl) && (cover_pl != defaultPlaylistName || app.playlist[0]);
        ui::section("COVER");
        const bool artwork_busy = busy || app.artwork_preview_loading;
        if (cover_named && !artwork_busy && app.artwork_preview_label != cover_pl &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            request_playlist_preview(app, cover_pl);
        const float cover = S(92.0f);
        const ImVec2 cover_at = ImGui::GetCursorScreenPos();
        if (app.artwork_preview && app.artwork_preview_label == cover_pl && !app.artwork_preview_loading) {
            ImGui::Image(app.artwork_preview, ImVec2(cover, cover));
        } else {
            ImGui::Dummy(ImVec2(cover, cover));
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(cover_at, ImVec2(cover_at.x + cover, cover_at.y + cover),
                                ImGui::GetColorU32(ImGuiCol_FrameBg), S(6));
            const char* text = !cover_named ? "Name the\nplaylist" : app.artwork_preview_loading ? "Loading..." : "No cover";
            const ImVec2 size = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(cover_at.x + (cover - size.x) * 0.5f, cover_at.y + (cover - size.y) * 0.5f),
                          ImGui::GetColorU32(ui::muted), text);
        }
        if (ImGui::IsItemHovered() && !app.artwork_preview_error.empty() && app.artwork_preview_label == cover_pl)
            ImGui::SetTooltip("%s", app.artwork_preview_error.c_str());
        ImGui::SameLine();
        ImGui::BeginGroup();
        {
            const float w = ImGui::GetContentRegionAvail().x;
            const char* mode = "Automatic";
            std::string chosen;
            if (const auto it = app.playlist_artwork.find(cover_pl); it != app.playlist_artwork.end() && !it->second.empty()) {
                chosen = narrow(it->second.filename().wstring());
                mode = chosen.c_str();
            } else if (app.generated_playlist_artwork.contains(cover_pl)) mode = "Text cover";
            ImGui::TextColored(ui::muted, "%s", ui::fit_middle(mode, w).c_str());
            ImGui::BeginDisabled(artwork_busy || !cover_named);
            if (ImGui::Button("Image...", ImVec2(w, 0))) {
                const auto files = pick(window, false, true);
                if (!files.empty()) {
                    app.playlist_artwork[cover_pl] = files[0];
                    app.generated_playlist_artwork.erase(cover_pl);
                    request_playlist_preview(app, cover_pl);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use an image of your own");
            if (ImGui::Button("Text", ImVec2((w - style.ItemSpacing.x) * 0.5f, 0))) {
                app.generated_playlist_artwork.insert(cover_pl);
                app.playlist_artwork[cover_pl].clear();
                request_playlist_preview(app, cover_pl);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Generate a cover with the playlist's name");
            ImGui::SameLine();
            if (ImGui::Button("Auto", ImVec2(-1, 0))) {
                app.playlist_artwork[cover_pl].clear();
                app.generated_playlist_artwork.erase(cover_pl);
                request_playlist_preview(app, cover_pl);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the first song's album art");
            ImGui::EndDisabled();
        }
        ImGui::EndGroup();
        ImGui::BeginDisabled(busy || app.rows.empty());
        if (ui::ghost_button("Song artwork...", ImVec2(-1, 0))) app.show_artwork_modal = true;
        ImGui::EndDisabled();

        ui::section("AUDIO");
        ImGui::BeginDisabled(busy);
        ImGui::SetNextItemWidth(half);
        if (ImGui::BeginCombo("##bitrate", (std::string(bitrates[app.bitrate]) + " kbps").c_str())) {
            for (int i = 0; i < 5; ++i)
                if (ImGui::Selectable((std::string(bitrates[i]) + " kbps").c_str(), app.bitrate == i)) app.bitrate = i;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opus audio bitrate");
        ImGui::SameLine();
        ImGui::Checkbox("Normalize", &app.normalize);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Equalise track loudness using EBU R128 (-15 LUFS), like the game's own songs");
        ImGui::EndDisabled();

        ImGui::EndChild(); // sidebar_scroll

        // Build and export, pinned to the bottom.
        ImGui::Dummy(ImVec2(0, style.ItemSpacing.y * 0.5f));
        ImGui::BeginDisabled(busy || !block.empty());
        if (ui::primary_button("Build mod", ImVec2(-1, S(42)))) {
            set_status(app, "");
            std::set<std::string> names;
            if (app.playlist[0]) names.insert(app.playlist.data());
            for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
            const bool any_automatic = std::any_of(names.begin(), names.end(), [&](const std::string& name) {
                return !(app.playlist_artwork.contains(name) && !app.playlist_artwork[name].empty()) &&
                       !app.generated_playlist_artwork.contains(name);
            });
            if (any_automatic) app.show_playlist_artwork_prompt = true;
            else build(app);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", block.empty() ? ("Build into " + narrow(output_folder(app).wstring())).c_str() : block.c_str());
        if (ImGui::Button("Export Thunderstore...", ImVec2(-1, 0))) {
            app.show_export_ts = true;
            if (!app.ts_description[0]) copy_text(app.ts_description, "Adds " + std::to_string(app.rows.size()) + " song(s) to skate.");
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // ---- Main pane: the songs ---------------------------------------------------------------------
    ImGui::SameLine(0, S(16));
    ImGui::BeginGroup();
    const float main_w = ImGui::GetContentRegionAvail().x; // SameLine offsets below are relative to this group
    const float status_h = ImGui::GetFrameHeight() + S(10);
    if (app.rows.empty()) {
        ImGui::BeginChild("empty", ImVec2(0, ImGui::GetContentRegionAvail().y - status_h));
        if (ui::begin_card("empty_state_card", ImVec2(S(640), S(330)))) {
            const float card_avail = ImGui::GetContentRegionAvail().x;
            const auto centred = [&](const char* text, const ImVec4& color) {
                const float w = ImGui::CalcTextSize(text).x;
                if (card_avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (card_avail - w) * 0.5f);
                ImGui::TextColored(color, "%s", text);
            };
            // An equalizer graphic.
            const float icon_w = S(72.0f), icon_h = S(42.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (card_avail - icon_w) * 0.5f));
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(icon_w, icon_h));
            const float heights[]{10.0f, 18.0f, 28.0f, 38.0f, 42.0f, 38.0f, 28.0f, 18.0f, 10.0f};
            const float bar_w = S(4.5f), bar_gap = S(3.8f);
            const float start_x = p0.x + (icon_w - (9 * bar_w + 8 * bar_gap)) * 0.5f;
            for (int b = 0; b < 9; ++b) {
                const float bh = heights[b] * (icon_h / 42.0f);
                const float bx = start_x + static_cast<float>(b) * (bar_w + bar_gap);
                const float t = 1.0f - std::abs(static_cast<float>(b) - 4.0f) / 5.0f;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(bx, p0.y + (icon_h - bh) * 0.5f), ImVec2(bx + bar_w, p0.y + (icon_h + bh) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.18f + 0.14f * t, 0.52f + 0.28f * t, 0.92f + 0.08f * t, 0.70f + 0.30f * t)), S(2.5f));
            }
            ImGui::Dummy(ImVec2(0, S(10)));
            if (ui::heading_font) ImGui::PushFont(ui::heading_font);
            centred("Drop songs or folders here", ImGui::GetStyleColorVec4(ImGuiCol_Text));
            if (ui::heading_font) ImGui::PopFont();
            ImGui::Spacing();
            centred(("Drag music in from " + std::string(file_manager) + ", or browse for it.").c_str(), ui::muted);
            centred("MP3, FLAC, WAV, OGG, Opus, AAC, M4A, AIFF and anything else ffmpeg reads.", ui::muted);
            ImGui::Dummy(ImVec2(0, S(16)));
            const float w1 = S(170), w2 = S(150);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (card_avail - w1 - w2 - S(12)) * 0.5f));
            ImGui::BeginDisabled(busy);
            if (ui::primary_button("Browse songs...", ImVec2(w1, S(36)))) add_files(app, pick(window, false));
            ImGui::SameLine(0, S(12));
            if (ImGui::Button("Add folder...", ImVec2(w2, S(36)))) add_files(app, pick(window, true));
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0, S(14)));
            centred("Tags and embedded album covers are read automatically.", ui::muted);
        }
        ui::end_card();
        ImGui::EndChild();
    } else {
        // Header: what is shown, how much of it, and adding more.
        const std::string title = app.active_playlist_filter.empty() ? "All songs" : app.active_playlist_filter;
        std::size_t shown = 0;
        double shown_seconds = 0;
        bool all_scanned = true;
        for (const auto& r : app.rows) {
            const std::string pl = r.playlist[0] ? r.playlist.data() : defaultPlaylistName;
            if (!app.active_playlist_filter.empty() && pl != app.active_playlist_filter) continue;
            ++shown;
            if (r.scanned) shown_seconds += r.seconds;
            else all_scanned = false;
        }
        const float add_w = S(118);
        ImGui::BeginGroup();
        ui::heading(title.c_str());
        ImGui::TextColored(ui::muted, "%zu song%s \xC2\xB7 %s%s", shown, shown == 1 ? "" : "s", ui::duration(shown_seconds).c_str(),
                           all_scanned ? "" : " \xC2\xB7 reading tags...");
        ImGui::EndGroup();
        ImGui::SameLine(main_w - add_w * 2 - style.ItemSpacing.x);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Add songs...", ImVec2(add_w, 0))) add_files(app, pick(window, false));
        if (!app.active_playlist_filter.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("Add songs directly into \"%s\"", app.active_playlist_filter.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Add folder...", ImVec2(add_w, 0))) add_files(app, pick(window, true));
        if (!app.active_playlist_filter.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("Add a folder's songs directly into \"%s\"", app.active_playlist_filter.c_str());
        ImGui::EndDisabled();
        ImGui::Spacing();

        // The table. Drag a song by its number to reorder it, or onto a playlist in the sidebar.
        std::optional<std::pair<std::size_t, std::size_t>> up_target, down_target, drag_move;
        std::optional<std::size_t> remove;
        const bool many_playlists = unique_playlists.size() > 1 || !app.custom_playlists.empty();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(6), S(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginTable("songs", 8,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_PadOuterX,
                              ImVec2(0, ImGui::GetContentRegionAvail().y - status_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, S(30));
            ImGui::TableSetupColumn("Artist", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Playlist", ImGuiTableColumnFlags_WidthStretch | (many_playlists ? 0 : ImGuiTableColumnFlags_Disabled), 1.0f);
            ImGui::TableSetupColumn("Art", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, S(32));
            ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, S(58));
            ImGui::TableSetupColumn("Problems", ImGuiTableColumnFlags_WidthStretch | (blocked ? 0 : ImGuiTableColumnFlags_Disabled), 0.9f);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, S(118));
            ImGui::TableHeadersRow();

            // Cells read like a list until hovered or edited.
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::transparent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

            static int active_context_row = -1;
            for (std::size_t i = 0; i < app.rows.size(); ++i) {
                auto& row = app.rows[i];
                const auto filtered_out = [&](std::size_t k) {
                    const std::string pl = app.rows[k].playlist[0] ? app.rows[k].playlist.data() : defaultPlaylistName;
                    return !app.active_playlist_filter.empty() && pl != app.active_playlist_filter;
                };
                if (filtered_out(i)) continue;
                std::optional<std::size_t> prev_matching, next_matching;
                for (std::size_t k = i; k > 0; --k) if (!filtered_out(k - 1)) { prev_matching = k - 1; break; }
                for (std::size_t k = i + 1; k < app.rows.size(); ++k) if (!filtered_out(k)) { next_matching = k; break; }

                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow(0, ImGui::GetFrameHeight());

                // Number: the row's handle for dragging, its menu and its details.
                ImGui::TableNextColumn();
                const bool has_problems = !problems[i].empty();
                char number[16];
                std::snprintf(number, sizeof(number), "%zu", i + 1);
                ImGui::PushStyleColor(ImGuiCol_Text, has_problems ? ui::warning : ui::muted);
                ImGui::Selectable(number, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, ImGui::GetFrameHeight()));
                ImGui::PopStyleColor();
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("RSMP_ROW", &i, sizeof(i));
                    ImGui::Text("%s - %s", row.artist.data(), row.title.data());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("RSMP_ROW"))
                        drag_move = std::make_pair(*static_cast<const std::size_t*>(payload->Data), i);
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(narrow(row.file.wstring()).c_str());
                    for (const auto& problem : problems[i]) ImGui::TextColored(ui::warning, "%s", problem.c_str());
                    ImGui::TextColored(ui::muted, "Drag to reorder or onto a playlist. Right-click for more.");
                    ImGui::EndTooltip();
                }

                if (active_context_row == static_cast<int>(i)) {
                    ImGui::OpenPopup("row_context");
                    active_context_row = -1;
                }
                if (ImGui::BeginPopupContextItem("row_context")) {
                    ImGui::TextDisabled("%s - %s", row.artist.data(), row.title.data());
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Assign to Playlist")) {
                        for (const auto& pl_name : unique_playlists) {
                            const bool is_curr = (row.playlist[0] ? row.playlist.data() == pl_name : pl_name == defaultPlaylistName);
                            if (ImGui::MenuItem((pl_name + (pl_name == defaultPlaylistName ? " (default)" : "")).c_str(), nullptr, is_curr))
                                assign_playlist(row, pl_name);
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("+ New Playlist...")) {
                            app.new_playlist_row_target = static_cast<int>(i);
                            app.new_playlist_input.fill(0);
                            app.show_new_playlist_modal = true;
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("Artwork")) {
                        if (ImGui::MenuItem("Choose Image...")) {
                            const auto files = pick(window, false, true);
                            if (!files.empty()) row.artwork = files[0];
                        }
                        if (!row.artwork.empty() && ImGui::MenuItem("Use Embedded / Automatic Artwork")) row.artwork.clear();
                        if (ImGui::MenuItem("Manage All Artwork...")) app.show_artwork_modal = true;
                        ImGui::EndMenu();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Move Up", nullptr, false, prev_matching.has_value() && !busy))
                        up_target = std::make_pair(i, *prev_matching);
                    if (ImGui::MenuItem("Move Down", nullptr, false, next_matching.has_value() && !busy))
                        down_target = std::make_pair(i, *next_matching);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Remove Song", nullptr, false, !busy)) remove = i;
                    ImGui::EndPopup();
                }

                ImGui::BeginDisabled(busy);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##artist", "Artist", row.artist.data(), row.artist.size());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##title", "Title", row.title.data(), row.title.size());

                // Playlist: type a name, or pick one from the arrow's menu.
                ImGui::TableNextColumn();
                const float arrow_w = ImGui::GetFrameHeight();
                ImGui::SetNextItemWidth(std::max(S(30.0f), ImGui::GetContentRegionAvail().x - arrow_w - style.ItemInnerSpacing.x));
                ImGui::InputTextWithHint("##playlist", defaultPlaylistName.c_str(), row.playlist.data(), row.playlist.size());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Playlist for this song.\nLeave blank to use \"%s\", or type or pick another.", defaultPlaylistName.c_str());
                ImGui::SameLine(0, style.ItemInnerSpacing.x);
                ImGui::PushStyleColor(ImGuiCol_Button, ui::transparent);
                if (ImGui::ArrowButton("##pl_arrow", ImGuiDir_Down)) ImGui::OpenPopup("PlaylistCellMenu");
                ImGui::PopStyleColor();
                if (ImGui::BeginPopup("PlaylistCellMenu")) {
                    ImGui::TextDisabled("Assign to Playlist");
                    ImGui::Separator();
                    for (const auto& pl_name : unique_playlists) {
                        const bool is_curr = (row.playlist[0] ? row.playlist.data() == pl_name : pl_name == defaultPlaylistName);
                        if (ImGui::MenuItem((pl_name + (pl_name == defaultPlaylistName ? " (default)" : "")).c_str(), nullptr, is_curr))
                            assign_playlist(row, pl_name);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("+ New Playlist...")) {
                        app.new_playlist_row_target = static_cast<int>(i);
                        app.new_playlist_input.fill(0);
                        app.show_new_playlist_modal = true;
                    }
                    ImGui::EndPopup();
                }

                // Art: a dot. Blue for a chosen image, green for embedded album art, hollow for none.
                ImGui::TableNextColumn();
                {
                    const float h = ImGui::GetFrameHeight();
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const float w = ImGui::GetContentRegionAvail().x;
                    if (ImGui::InvisibleButton("art", ImVec2(w, h))) {
                        const auto files = pick(window, false, true);
                        if (!files.empty()) row.artwork = files[0];
                    }
                    const ImVec2 centre(at.x + w * 0.5f, at.y + h * 0.5f);
                    auto* draw = ImGui::GetWindowDrawList();
                    const float r = S(5.0f) * (ImGui::IsItemHovered() ? 1.25f : 1.0f);
                    if (!row.artwork.empty()) draw->AddCircleFilled(centre, r, ImGui::GetColorU32(ui::accent));
                    else if (row.has_embedded_artwork) draw->AddCircleFilled(centre, r, ImGui::GetColorU32(ui::success));
                    else draw->AddCircle(centre, r, ImGui::GetColorU32(ui::muted), 0, S(1.5f));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\nClick to choose an image.",
                                          !row.artwork.empty() ? ("Cover: " + narrow(row.artwork.filename().wstring())).c_str()
                                          : row.has_embedded_artwork ? "Cover: the song's embedded album art"
                                                                     : "No cover");
                }
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (row.scanned) ImGui::TextUnformatted(ui::duration(row.seconds).c_str());
                else ImGui::TextDisabled("...");

                ImGui::TableNextColumn();
                if (has_problems) {
                    std::string text;
                    for (const auto& problem : problems[i]) text += (text.empty() ? "" : "; ") + problem;
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ui::warning, "%s", ui::fit_end(text, ImGui::GetContentRegionAvail().x).c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
                }

                // Actions: move, remove, more.
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(busy);
                ImGui::PushStyleColor(ImGuiCol_Button, ui::transparent);
                ImGui::BeginDisabled(!prev_matching.has_value());
                if (ImGui::ArrowButton("up", ImGuiDir_Up)) up_target = std::make_pair(i, *prev_matching);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Move up");
                ImGui::SameLine(0, S(2));
                ImGui::BeginDisabled(!next_matching.has_value());
                if (ImGui::ArrowButton("down", ImGuiDir_Down)) down_target = std::make_pair(i, *next_matching);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Move down");
                ImGui::PopStyleColor();
                ImGui::SameLine(0, S(2));
                const ImVec4 danger_bg(ui::danger.x, ui::danger.y, ui::danger.z, 0.55f);
                if (ui::ghost_button("x##remove", ImVec2(ImGui::GetFrameHeight(), 0), &danger_bg)) remove = i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this song");
                ImGui::SameLine(0, S(2));
                if (ui::ghost_button("...##more_actions", ImVec2(ImGui::GetFrameHeight(), 0))) active_context_row = static_cast<int>(i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("More (or right-click the row)");
                ImGui::EndDisabled();

                ImGui::PopID();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            if (shown == 0 && !app.active_playlist_filter.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::Dummy(ImVec2(0, S(8)));
                ImGui::TextDisabled("No songs in \"%s\" yet.", app.active_playlist_filter.c_str());
                ImGui::TextDisabled("Drop files here, use Add songs..., or drag songs onto it in the sidebar.");
                ImGui::Dummy(ImVec2(0, S(8)));
            }

            if (ImGui::BeginPopupContextWindow("table_empty_context", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
                if (ImGui::MenuItem("Add Songs...")) add_files(app, pick(window, false));
                if (ImGui::MenuItem("Add Folder...")) add_files(app, pick(window, true));
                ImGui::Separator();
                if (ImGui::MenuItem("+ New Playlist...")) {
                    app.new_playlist_row_target = -1;
                    app.new_playlist_input.fill(0);
                    app.show_new_playlist_modal = true;
                }
                if (ImGui::MenuItem("Manage Artwork...")) app.show_artwork_modal = true;
                ImGui::EndPopup();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(2);
        if (up_target) std::swap(app.rows[up_target->first], app.rows[up_target->second]);
        if (down_target) std::swap(app.rows[down_target->first], app.rows[down_target->second]);
        if (drag_move && drag_move->first != drag_move->second && drag_move->first < app.rows.size() && !busy) {
            auto moved = std::move(app.rows[drag_move->first]);
            app.rows.erase(app.rows.begin() + static_cast<std::ptrdiff_t>(drag_move->first));
            app.rows.insert(app.rows.begin() + static_cast<std::ptrdiff_t>(drag_move->second), std::move(moved));
        }
        if (remove) app.rows.erase(app.rows.begin() + static_cast<std::ptrdiff_t>(*remove));
    }

    // ---- Status bar: progress, the last result, or what Build is waiting for --------------------
    ImGui::Dummy(ImVec2(0, S(4)));
    if (busy) {
        std::lock_guard lock(app.mutex);
        ImGui::ProgressBar(app.progress, ImVec2(-S(110), 0), (app.job + (app.progress_text.empty() ? "" : ": " + app.progress_text)).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1, 0))) app.cancel = true;
    } else {
        ImGui::AlignTextToFramePadding();
        if (!app.status.empty()) ui::status_text(app.status, app.status_error);
        else if (!block.empty()) ImGui::TextColored(blocked ? ui::warning : ui::muted, "%s", block.c_str());
        else if (game_running()) ImGui::TextColored(ui::warning, "skate. is running: the mod takes effect the next time it starts.");
        else ImGui::TextColored(ui::muted, "Ready to build %zu song%s.", app.rows.size(), app.rows.size() == 1 ? "" : "s");
    }
    ImGui::EndGroup();

    if (app.show_playlist_artwork_prompt) {
        ImGui::OpenPopup("Playlist Artwork Setup");
        app.show_playlist_artwork_prompt = false;
        std::set<std::string> names;
        if (app.playlist[0]) names.insert(app.playlist.data());
        for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
        const auto pName = names.empty() ? "Playlist" : *names.begin();
        request_playlist_preview(app, pName);
    }
    if (ImGui::BeginPopupModal("Playlist Artwork Setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::set<std::string> names;
        if (app.playlist[0]) names.insert(app.playlist.data());
        for (const auto& row : app.rows) if (row.playlist[0]) names.insert(row.playlist.data());
        std::vector<std::string> unconfigured;
        for (const auto& name : names) {
            bool hasImage = app.playlist_artwork.contains(name) && !app.playlist_artwork[name].empty();
            bool hasGen = app.generated_playlist_artwork.contains(name);
            if (!hasImage && !hasGen) unconfigured.push_back(name);
        }

        ImGui::Spacing();
        ImGui::Text("Playlist artwork has not been selected.");
        ImGui::TextDisabled("skate. displays playlist covers in its in-game music menu.");
        ImGui::Spacing();

        static int selected_idx = 0;
        if (selected_idx >= static_cast<int>(unconfigured.size())) selected_idx = 0;

        const auto pName = unconfigured.empty()
            ? (app.playlist[0] ? std::string(app.playlist.data()) : "Playlist")
            : unconfigured[selected_idx];

        // Left side: Preview
        ImGui::BeginGroup();
        if (app.artwork_preview_loading) {
            ImGui::BeginChild("cover_preview_box", ImVec2(S(180), S(180)), true);
            ImGui::TextWrapped("Loading preview...");
            ImGui::EndChild();
        } else if (app.artwork_preview && app.artwork_preview_label == pName) {
            ImGui::Image(app.artwork_preview, ImVec2(S(180), S(180)));
        } else {
            ImGui::BeginChild("cover_preview_box", ImVec2(S(180), S(180)), true);
            ImGui::Spacing();
            ImGui::TextDisabled("No cover");
            if (!app.artwork_preview_error.empty()) {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(160));
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", app.artwork_preview_error.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
        }
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // Right side: controls
        ImGui::BeginGroup();
        const bool artwork_busy = busy || app.artwork_preview_loading;
        if (unconfigured.size() > 1) {
            std::vector<const char*> pl_ptrs;
            for (const auto& name : unconfigured) pl_ptrs.push_back(name.c_str());
            ImGui::BeginDisabled(artwork_busy);
            if (ImGui::Combo("Playlist", &selected_idx, pl_ptrs.data(), static_cast<int>(pl_ptrs.size()))) {
                const auto& curName = unconfigured[selected_idx];
                request_playlist_preview(app, curName);
            }
            ImGui::EndDisabled();
        } else {
            ImGui::Text("Playlist: %s", pName.c_str());
        }
        ImGui::Spacing();

        if (!app.playlist_artwork[pName].empty()) {
            ImGui::Text("Active: %s", narrow(app.playlist_artwork[pName].filename().wstring()).c_str());
        } else if (app.generated_playlist_artwork.contains(pName)) {
            ImGui::TextUnformatted("Active: Generated text cover");
        } else {
            ImGui::TextUnformatted("Active: Automatic (from first track)");
        }
        ImGui::Spacing();

        ImGui::BeginDisabled(artwork_busy);
        if (ImGui::Button("Generate Text Cover", ImVec2(S(200), S(28)))) {
            app.generated_playlist_artwork.insert(pName);
            app.playlist_artwork[pName].clear();
            request_playlist_preview(app, pName);
        }

        if (ImGui::Button("Choose Image...", ImVec2(S(200), S(28)))) {
            const auto files = pick(window, false, true);
            if (!files.empty()) {
                app.playlist_artwork[pName] = files[0];
                app.generated_playlist_artwork.erase(pName);
                request_playlist_preview(app, pName);
            }
        }

        if (ImGui::Button("Use Automatic", ImVec2(S(200), S(28)))) {
            app.playlist_artwork[pName].clear();
            app.generated_playlist_artwork.erase(pName);
            request_playlist_preview(app, pName);
        }

        if (unconfigured.size() > 1) {
            ImGui::Spacing();
            if (ImGui::Button("Generate Text for All", ImVec2(S(200), S(28)))) {
                for (const auto& name : unconfigured) {
                    app.generated_playlist_artwork.insert(name);
                    app.playlist_artwork[name].clear();
                }
                request_playlist_preview(app, pName);
            }
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginDisabled(artwork_busy);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.78f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.54f, 0.90f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.38f, 0.68f, 1.00f));
        if (ImGui::Button("Build", ImVec2(S(120), S(30)))) {
            build(app);
            clear_artwork_preview(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(100), S(30)))) {
            clear_artwork_preview(app);
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (app.show_export_ts) {
        ImGui::OpenPopup("Export Thunderstore Package");
        app.show_export_ts = false;
    }
    if (ImGui::BeginPopupModal("Export Thunderstore Package", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Spacing();
        ImGui::Text("Export a Thunderstore-compatible .zip package ready for upload.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(S(320));
        ImGui::InputText("Author / Namespace", app.ts_author.data(), app.ts_author.size());
        ImGui::SetNextItemWidth(S(320));
        ImGui::InputText("Version", app.ts_version.data(), app.ts_version.size());
        ImGui::SetNextItemWidth(S(320));
        ImGui::InputText("Description", app.ts_description.data(), app.ts_description.size());
        ImGui::Spacing();
        if (!app.ts_icon.empty()) {
            ImGui::Text("Icon: %s", narrow(app.ts_icon.filename().wstring()).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(app.ts_icon.wstring()).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Clear##ts_icon")) app.ts_icon.clear();
        } else {
            ImGui::TextDisabled("Icon: Default (auto-generated 256x256)");
            ImGui::SameLine();
            if (ImGui::Button("Browse...##ts_icon")) {
                const auto picked = pick(window, false, true);
                if (!picked.empty()) app.ts_icon = picked[0];
            }
        }
        ImGui::Spacing();

        const auto mod = output_folder(app);
        const auto defaultFolder = mod.parent_path();
        const auto effectiveFolder = !app.ts_output_folder.empty() ? app.ts_output_folder : defaultFolder;

        ImGui::Text("Output folder: %s", narrow(effectiveFolder.filename().wstring().empty() ? effectiveFolder.wstring() : effectiveFolder.filename().wstring()).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(effectiveFolder.wstring()).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Browse...##ts_out")) {
            const auto folders = pick(window, true);
            if (!folders.empty()) app.ts_output_folder = folders[0];
        }
        if (!app.ts_output_folder.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Reset##ts_out")) app.ts_output_folder.clear();
        }

        const auto authorStr = app.ts_author[0] ? app.ts_author.data() : "Author";
        const auto versionStr = app.ts_version[0] ? app.ts_version.data() : "1.0.0";
        const auto modNameStr = app.name[0] ? app.name.data() : "ReSkateMusic";
        const auto expectedZipName = std::string(authorStr) + "-" + modNameStr + "-" + versionStr + ".zip";
        ImGui::TextDisabled("Package: %s", expectedZipName.c_str());

        ImGui::Spacing();
        ImGui::Checkbox(("Show in " + std::string(file_manager) + " when finished").c_str(), &app.ts_open_explorer);
        ImGui::Checkbox("Include \"Packaged with ReSkate Music Packer\" link in README", &app.ts_readme_credit);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.45f, 0.78f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.54f, 0.90f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.38f, 0.68f, 1.00f));
        if (ImGui::Button("Export ZIP", ImVec2(S(140), S(30)))) {
            try {
                if (!fs::exists(mod / L"layout.toc")) {
                    set_status(app, "Build the mod first before exporting.", true);
                } else {
                    music::ThunderstoreOptions opts;
                    opts.author = app.ts_author.data();
                    opts.version = app.ts_version.data();
                    opts.description = app.ts_description.data();
                    opts.icon = app.ts_icon;
                    opts.output = effectiveFolder;
                    opts.readme_credit = app.ts_readme_credit;
                    const auto zip = music::export_thunderstore(mod, opts);
                    set_status(app, "Exported: " + narrow(zip.wstring()));
                    if (app.ts_open_explorer) platform::reveal(zip);
                }
            } catch (const std::exception& error) {
                set_status(app, error.what(), true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(100), S(30)))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void frame(App& app, NativeWindow window) {
    apply_scans(app);
    apply_artwork_preview(app);
    {
        std::optional<FfmpegInstallResult> install;
        {
            std::lock_guard lock(app.mutex);
            install = std::move(app.ffmpeg_install);
            app.ffmpeg_install.reset();
        }
        if (install) {
            if (install->ok && find_ffmpeg(install->folder)) {
                app.settings.ffmpeg = install->folder;
                save_settings(app.settings);
                app.ffmpeg = true;
                set_status(app, "ffmpeg is ready.");
            } else if (install->ok) {
                set_status(app, "The ffmpeg download finished, but " + ffmpeg_pair + " were not found.", true);
            } else {
                set_status(app, install->error.empty() ? "The ffmpeg download failed." : install->error, true);
            }
        }
    }
    {
        std::lock_guard lock(app.mutex);
        if (app.finished) {
            set_status(app, app.finished->second, !app.finished->first);
            if (app.finished->first) refresh_external_songs(app);
            app.finished.reset();
        }
    }
    {
        std::vector<fs::path> dropped;
        {
            std::lock_guard lock(app.dropped_mutex);
            dropped.swap(app.dropped);
        }
        if (!dropped.empty() && game_folder(app.settings.game) && app.ffmpeg) add_files(app, dropped);
    }
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("music", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (!game_folder(app.settings.game)) setup_page(app, window);
    else if (!app.ffmpeg) ffmpeg_page(app, window);
    else songs_page(app, window);
    ImGui::End();
    settings_modal(app, window);
}

#ifdef _WIN32
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    switch (message) {
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (g_app) {
            std::lock_guard lock(g_app->dropped_mutex);
            for (UINT i = 0; i < count; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                g_app->dropped.emplace_back(path);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
#endif

int run_cli(int argc, wchar_t** argv) {
    music::PackOptions options;
    std::vector<fs::path> positional;
    bool thunderstore = false;
    bool get_ffmpeg = false;
    fs::path ffmpeg_dir;
    std::map<std::string, fs::path> trackArtwork;
    std::string author = "Author", version = "1.0.0";
    bool readme_credit = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            std::printf("ReSkate Music Packer (CLI mode)\n\n"
                        "Usage:\n"
                        "  %s <game folder> <song folder> [output folder] [options]\n\n"
                        "Options:\n"
                        "  --name <mod name>          Display name of the mod (default: folder name)\n"
                        "  --playlist <playlist>      Playlist name shown in-game (default: folder name)\n"
                        "  --bitrate <kbps>           Opus bitrate 64-320 kbps (default: 192)\n"
                        "  --no-normalize             Disable EBU R128 loudness normalization\n"
                        "  --thunderstore             Export a Thunderstore-ready zip package\n"
                        "  --author <name>            Thunderstore package author (default: Author)\n"
                        "  --version <x.y.z>          Thunderstore package version (default: 1.0.0)\n"
                        "  --no-readme-credit         Omit packer credit link from the README\n"
                        "  --playlist-artwork <name> <image>  Cover for a playlist\n"
                        "  --track-artwork <id> <image>       Cover for Artist - Title\n"
                        "  --generate-playlist-artwork <name> Text cover for a playlist\n"
                        "  --get-ffmpeg [folder]      Download ffmpeg into a folder (default: next to this exe or\n"
                        "                             %%LOCALAPPDATA%%) and print where it went (Windows only)\n"
                        "  --gui                      Launch graphical user interface\n"
                        "  --help, -h                 Show this help text\n", executable_name.c_str());
            return 0;
        }
        else if (arg == L"--name" && i + 1 < argc) options.name = narrow(argv[++i]);
        else if (arg == L"--bitrate" && i + 1 < argc) options.bitrate = std::stoi(argv[++i]);
        else if (arg == L"--playlist" && i + 1 < argc) options.playlist = narrow(argv[++i]);
        else if (arg == L"--no-normalize") options.normalize = false;
        else if (arg == L"--thunderstore") thunderstore = true;
        else if (arg == L"--author" && i + 1 < argc) author = narrow(argv[++i]);
        else if (arg == L"--version" && i + 1 < argc) version = narrow(argv[++i]);
        else if (arg == L"--no-readme-credit") readme_credit = false;
        else if (arg == L"--playlist-artwork" && i + 2 < argc) {
            const auto name = narrow(argv[++i]);
            options.playlist_artwork[name] = argv[++i];
        }
        else if (arg == L"--track-artwork" && i + 2 < argc) {
            const auto id = narrow(argv[++i]);
            trackArtwork[id] = argv[++i];
        }
        else if (arg == L"--generate-playlist-artwork" && i + 1 < argc)
            options.generated_playlist_artwork.insert(narrow(argv[++i]));
        else if (arg == L"--get-ffmpeg") {
            get_ffmpeg = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-' && argv[i + 1][0] != L'\0') ffmpeg_dir = argv[++i];
        }
        else if (arg == L"--gui") {}
        else positional.emplace_back(arg);
    }
    if (get_ffmpeg) {
        if (!music::ffmpeg_download_supported()) {
            std::fprintf(stderr, "error: --get-ffmpeg downloads a Windows build; install ffmpeg with your package manager "
                                 "(brew install ffmpeg, apt install ffmpeg, ...) instead\n");
            return 2;
        }
        try {
            const fs::path directory = ffmpeg_dir.empty() ? music::default_install_dir() : ffmpeg_dir;
            std::printf("Downloading ffmpeg into %s ...\n", narrow(directory.wstring()).c_str());
            music::ensure_ffmpeg(directory, music::ffmpeg_url(), music::ffmpeg_sha256(),
                [](const music::DownloadProgress& step) {
                    if (step.total)
                        std::printf("\r  %llu / %llu MB", static_cast<unsigned long long>(step.received >> 20),
                                    static_cast<unsigned long long>(step.total >> 20));
                    else
                        std::printf("\r  %llu MB", static_cast<unsigned long long>(step.received >> 20));
                    std::fflush(stdout);
                });
            std::printf("\nffmpeg -> %s\n", narrow(directory.wstring()).c_str());
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "error: %s\n", error.what());
            return 2;
        }
    }
    if (positional.size() < 2 || positional.size() > 3 || options.bitrate < 64 || options.bitrate > 320) {
        std::fprintf(stderr, "Usage: %s <game folder> <song folder> [output folder] [options]\n"
                             "Run '%s --help' for details, or run with no arguments for GUI.\n",
                     executable_name.c_str(), executable_name.c_str());
        return 1;
    }
    const auto& songFolder = positional[1];
    if (options.playlist.empty()) options.playlist = narrow(fs::path(songFolder).filename().wstring());
    if (!music::usable_name(options.playlist)) {
        std::fprintf(stderr, "error: the playlist name is empty, too long, or has control characters\n");
        return 1;
    }
    options.game = positional[0];
    options.output = positional.size() > 2 ? positional[2] : songFolder.parent_path() / (songFolder.filename().wstring() + L"_mod");
    try {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(songFolder)) {
            if (!entry.is_regular_file()) continue;
            static const std::set<std::string> imageExtensions{".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".avif", ".tif", ".tiff", ".ico"};
            if (imageExtensions.contains(lower(narrow(entry.path().extension().wstring())))) continue;
            const auto matchesImage = [&](const auto& item) {
                std::error_code error;
                return fs::equivalent(entry.path(), item.second, error);
            };
            if (std::any_of(trackArtwork.begin(), trackArtwork.end(), matchesImage) ||
                std::any_of(options.playlist_artwork.begin(), options.playlist_artwork.end(), matchesImage)) continue;
            files.push_back(entry.path());
        }
        std::ranges::sort(files);
        if (files.empty()) throw std::runtime_error("No songs in " + narrow(songFolder.wstring()));

        auto songs = music::scan(files);
        for (const auto& [id, image] : trackArtwork) {
            const auto found = std::find_if(songs.begin(), songs.end(), [&](const auto& song) {
                return song.artist + " - " + song.title == id;
            });
            if (found == songs.end()) throw std::runtime_error("Artwork names an unknown track: " + id);
            found->artwork = image;
        }
        for (const auto& [name, image] : options.playlist_artwork) {
            if (name != options.playlist) throw std::runtime_error("Artwork names an unknown playlist: " + name);
        }
        for (const auto& name : options.generated_playlist_artwork)
            if (name != options.playlist) throw std::runtime_error("Generated artwork names an unknown playlist: " + name);
        const auto result = music::pack(options, songs, [&](const music::Progress& step) {
            const std::string stage = step.stage;
            if (stage == "encoding") std::printf("%s - %s\n", songs[step.song].artist.c_str(), songs[step.song].title.c_str());
            else if (stage == "encoded") std::printf("  %s\n", step.detail.c_str());
        });
        std::printf("%zu song(s), %zu KB of audio -> %s\n", result.songs, result.audioKb, narrow(result.output.wstring()).c_str());
        if (thunderstore) {
            const auto zip = music::export_thunderstore(result.output, {
                .author = author,
                .version = version,
                .readme_credit = readme_credit
            });
            std::printf("Thunderstore package -> %s\n", narrow(zip.wstring()).c_str());
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}

} // namespace



#ifdef _WIN32
int run_gui(HINSTANCE instance, int /*cmd_show*/) {

    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_scale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
    const auto work_width = static_cast<float>(work.right - work.left);
    const auto work_height = static_cast<float>(work.bottom - work.top);
    if (work_width > 0 && work_height > 0)
        g_scale = std::clamp(std::min(work_width * 0.98f / window_width, work_height * 0.96f / window_height),
                             0.62f, g_scale);

    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = L"ReSkateMusicMaker";
    RegisterClassExW(&type);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const int width = static_cast<int>(S(window_width));
    const int height = static_cast<int>(S(window_height));
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, style, FALSE);
    const int win_w = rect.right - rect.left;
    const int win_h = rect.bottom - rect.top;
    const int win_x = work.left + (static_cast<int>(work_width) - win_w) / 2;
    const int win_y = work.top + (static_cast<int>(work_height) - win_h) / 2;
    const auto window = CreateWindowExW(0, type.lpszClassName, L"ReSkate Music Packer", style, win_x, win_y,
                                        win_w, win_h, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    const auto font = fs::path(windows) / L"Fonts" / L"segoeui.ttf";
    if (fs::exists(font)) io.Fonts->AddFontFromFileTTF(narrow(font.wstring()).c_str(), S(18.0f));
    if (const auto bold = fs::path(windows) / L"Fonts" / L"segoeuib.ttf"; fs::exists(font) && fs::exists(bold))
        ui::heading_font = io.Fonts->AddFontFromFileTTF(narrow(bold.wstring()).c_str(), S(24.0f));
    ImGui::StyleColorsDark();
    apply_theme();
    ImGui_ImplWin32_Init(window);
    Renderer renderer;
    if (!renderer.init(window)) {
        MessageBoxW(nullptr, L"This PC's graphics driver cannot draw the window.", L"ReSkate Music Packer", MB_ICONERROR);
        return 1;
    }

    auto app_storage = std::make_unique<App>();
    auto& app = *app_storage;
    app.renderer = &renderer;
    g_app = &app;
    app.settings = load_settings();
    app.ffmpeg = find_ffmpeg(app.settings.ffmpeg);
    refresh_external_songs(app);
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOWNORMAL);

    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(window)) { Sleep(50); continue; }
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        frame(app, window);
        ImGui::Render();
        renderer.render();
    }
    app.cancel = true;
    if (app.worker.joinable()) app.worker.join();
    if (app.artwork_preview) renderer.release_texture(app.artwork_preview);
    g_app = nullptr;
    renderer.shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(window);
    if (com) CoUninitialize();
    return 0;
}


int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int cmd_show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool force_gui = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--gui") force_gui = true;
    }
    if (argc > 1 && !force_gui) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            std::ios::sync_with_stdio();
        }
        const int result = run_cli(argc, argv);
        LocalFree(argv);
        return result;
    }
    LocalFree(argv);
    return run_gui(instance, cmd_show);
}
#else // macOS and Linux: an SDL2 window, the same pages, the same CLI.

namespace {
// The UI font: what the system uses for plain sans text, at hand without fontconfig on macOS.
fs::path ui_font(bool bold = false) {
#ifdef __APPLE__
    for (const char* file : bold ? std::initializer_list<const char*>{"/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"}
                                 : std::initializer_list<const char*>{"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"})
        if (fs::exists(file)) return file;
#else
    if (std::string file; platform::run_process({"fc-match", "-f", "%{file}", bold ? "sans-serif:bold" : "sans-serif"}, &file) == 0 &&
                          fs::exists(file))
        return file;
    for (const char* file : bold ? std::initializer_list<const char*>{"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                                                      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"}
                                 : std::initializer_list<const char*>{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                                                      "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                                                      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"})
        if (fs::exists(file)) return file;
#endif
    return {}; // ImGui's built-in font
}
} // namespace

// `files` are songs to start with (`--gui <files>`). RSMP_CAPTURE=<png> saves one settled frame and
// quits; RSMP_CAPTURE_OPEN=settings|artwork|export|playlist opens that dialog first. Both are for
// checking the UI from scripts and CI, where there is no screen to look at.
int run_gui(const std::vector<fs::path>& files) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "ReSkate Music Packer: cannot open a window: %s\n", SDL_GetError());
        return 1;
    }
    // macOS lays out in points and SDL renders them at the display's density. Linux desktops report
    // their scale as DPI instead, which sizes the layout the way the Windows build does.
#ifndef __APPLE__
    if (float dpi{}; SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) == 0 && dpi > 0) g_scale = std::max(1.0f, dpi / 96.0f);
#endif
    if (SDL_Rect work{}; SDL_GetDisplayUsableBounds(0, &work) == 0 && work.w > 0 && work.h > 0)
        g_scale = std::clamp(std::min(static_cast<float>(work.w) * 0.98f / window_width,
                                      static_cast<float>(work.h) * 0.96f / window_height), 0.62f, g_scale);
    auto* window = SDL_CreateWindow("ReSkate Music Packer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    static_cast<int>(S(window_width)), static_cast<int>(S(window_height)),
                                    SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "ReSkate Music Packer: cannot open a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    Renderer renderer;
    if (!renderer.init(window)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ReSkate Music Packer", "This system's graphics driver cannot draw the window.", window);
        return 1;
    }
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer.sdl());
    // Rasterize the font at the display's pixel density and draw it at point size, so text is sharp on Retina.
    int points{}, pixels{};
    SDL_GetWindowSize(window, &points, nullptr);
    SDL_GetRendererOutputSize(renderer.sdl(), &pixels, nullptr);
    const float density = points > 0 && pixels > points ? static_cast<float>(pixels) / static_cast<float>(points) : 1.0f;
    if (const auto font = ui_font(); !font.empty()) {
        io.Fonts->AddFontFromFileTTF(font.string().c_str(), S(18.0f) * density);
        if (const auto bold = ui_font(true); !bold.empty())
            ui::heading_font = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), S(24.0f) * density);
        io.FontGlobalScale = 1.0f / density;
    }
    ImGui::StyleColorsDark();
    apply_theme();

    auto app_storage = std::make_unique<App>();
    auto& app = *app_storage;
    app.renderer = &renderer;
    g_app = &app;
    app.settings = load_settings();
    app.ffmpeg = find_ffmpeg(app.settings.ffmpeg);
    refresh_external_songs(app);
    // `--gui <mod folder>` reopens a mod; anything else is songs to add.
    if (files.size() == 1 && fs::is_regular_file(files[0] / L"reskate-music-project.json") && game_folder(app.settings.game) && app.ffmpeg)
        open_mod(app, files[0]);
    else if (!files.empty() && game_folder(app.settings.game) && app.ffmpeg) add_files(app, files);
    const auto capture = platform::environment("RSMP_CAPTURE");
    if (const auto open = platform::environment("RSMP_CAPTURE_OPEN"); !capture.empty()) {
        app.settings_open = open == "settings";
        app.show_artwork_modal = open == "artwork";
        app.show_export_ts = open == "export";
        app.show_new_playlist_modal = open == "playlist";
    }
    int frames = 0;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
            if (event.type == SDL_DROPFILE) {
                {
                    std::lock_guard lock(app.dropped_mutex);
                    app.dropped.emplace_back(event.drop.file);
                }
                SDL_free(event.drop.file);
            }
        }
        if (!running) break;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        frame(app, window);
        ImGui::Render();
        // Capture once background work has settled and the layout has had a few frames to size itself.
        const bool capture_now = !capture.empty() && ++frames > 30 && !app.busy && !app.artwork_preview_loading;
        if (capture_now) renderer.capture_next(capture);
        renderer.render();
        if (capture_now) running = false;
    }
    app.cancel = true;
    if (app.worker.joinable()) app.worker.join();
    if (app.artwork_preview) renderer.release_texture(app.artwork_preview);
    g_app = nullptr;
    renderer.shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

int main(int argc, char** argv) {
    platform::add_common_program_folders();
    bool force_gui = false;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--gui") force_gui = true;
    // macOS adds -psn_... when an app bundle is opened from Finder; that is not a CLI run.
    const bool cli = argc > 1 && !force_gui && !std::string_view(argv[1]).starts_with("-psn_");
    if (cli) {
        std::vector<std::wstring> wide;
        for (int i = 0; i < argc; ++i) wide.push_back(platform::widen(argv[i]));
        std::vector<wchar_t*> pointers;
        for (auto& arg : wide) pointers.push_back(arg.data());
        return run_cli(argc, pointers.data());
    }
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i)
        if (const std::string_view arg = argv[i]; arg != "--gui" && !arg.starts_with("-psn_")) files.emplace_back(arg);
    return run_gui(files);
}
#endif
