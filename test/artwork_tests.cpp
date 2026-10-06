// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "packer.h"
#include "platform.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

// Third-party, compiled here as static functions: silence warnings about the parts not used.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505 4100 4244 4456 4457 4701 4703)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "stb/stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace {
namespace fs = std::filesystem;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
void write(const fs::path& path, const std::vector<std::byte>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Could not write artwork fixture");
}
void run(const std::vector<std::string>& args) {
    if (platform::run_process(args) != 0) throw std::runtime_error("Artwork fixture command failed: " + platform::command_line(args));
}
std::string arg(const fs::path& path) { return platform::narrow(path.wstring()); }
// A decoded PNG as RGBA; width 0 when it does not decode.
struct Image {
    int width{}, height{};
    std::vector<unsigned char> rgba;
};
Image decode(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    Image image;
    int channels{};
    if (auto* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &image.width, &image.height, &channels, 4)) {
        image.rgba.assign(pixels, pixels + static_cast<std::size_t>(image.width) * image.height * 4);
        stbi_image_free(pixels);
    } else image.width = image.height = 0;
    return image;
}
bool square_png(const fs::path& file) {
    const auto image = decode(file);
    return image.width == 512 && image.height == 512;
}
std::array<unsigned char, 4> pixel(const fs::path& file, int x, int y) {
    const auto image = decode(file);
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) throw std::runtime_error("Could not inspect PNG pixel");
    std::array<unsigned char, 4> color{};
    std::copy_n(image.rgba.begin() + (static_cast<std::ptrdiff_t>(y) * image.width + x) * 4, 4, color.begin());
    return color;
}
bool dark(const std::array<unsigned char, 4>& color) {
    return color[0] >= 33 && color[0] <= 39 && color[1] >= 29 && color[1] <= 35 && color[2] >= 50 && color[2] <= 56;
}
bool red(const std::array<unsigned char, 4>& color) { return color[0] > 240 && color[1] < 10 && color[2] < 10; }
}
int run_tests(const std::vector<fs::path>& argv) try {
    const auto argc = argv.size();
    const auto root = fs::temp_directory_path() /
        ("MusicArtworkTests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    const auto generated = music::playlist_artwork_png("Late Night Sessions");
    write(root / L"generated.png", generated);
    check(square_png(root / L"generated.png"), "generated cover decodes as 512x512");
    check(generated == music::playlist_artwork_png("Late Night Sessions"), "same title produces the same cover");
    check(generated != music::playlist_artwork_png("Morning Mix"), "playlist text changes the rendered PNG");
    write(root / L"unicode.png", music::playlist_artwork_png("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E & Caf\xC3\xA9"));
    check(square_png(root / L"unicode.png"), "Unicode title renders");
    write(root / L"long.png", music::playlist_artwork_png(std::string(255, 'W')));
    check(square_png(root / L"long.png"), "long unbroken title renders");
    try { music::playlist_artwork_png(""); check(false, "empty title is rejected"); }
    catch (const std::runtime_error&) {}
    if (argc > 1) {
        write(argv[1], generated);
        const auto directory = fs::path(argv[1]).parent_path();
        write(directory / L"artwork-unicode-preview.png", music::playlist_artwork_png("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E & Caf\xC3\xA9"));
        write(directory / L"artwork-long-preview.png", music::playlist_artwork_png(std::string(255, 'W')));
    }
    const auto rectangle = root / L"rectangle.png";
    run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=320x180", "-frames:v", "1", "-update", "1", arg(rectangle)});
    const auto fitted = music::image_artwork_png(rectangle);
    write(root / L"fitted.png", fitted);
    check(square_png(root / L"fitted.png") && dark(pixel(root / L"fitted.png", 256, 95)) &&
          red(pixel(root / L"fitted.png", 256, 115)) && red(pixel(root / L"fitted.png", 0, 256)),
          "landscape artwork keeps its proportions with dark top and bottom padding");
    if (argc > 1) write(fs::path(argv[1]).parent_path() / L"artwork-fit-preview.png", fitted);
    run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=90x160", "-frames:v", "1", "-update", "1", arg(root / "portrait.png")});
    write(root / L"portrait-fit.png", music::image_artwork_png(root / L"portrait.png"));
    check(dark(pixel(root / L"portrait-fit.png", 95, 256)) && red(pixel(root / L"portrait-fit.png", 115, 256)) &&
          red(pixel(root / L"portrait-fit.png", 256, 0)), "portrait artwork keeps its proportions with side padding");
    const auto plain = root / L"plain.mp3", tagged = root / L"tagged.mp3", flac = root / L"tagged.flac";
    run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-metadata", "artist=Artist",
         "-metadata", "title=Track", arg(plain)});
    run({"ffmpeg", "-y", "-v", "error", "-i", arg(plain), "-i", arg(rectangle), "-map", "0:a", "-map", "1:v", "-c", "copy",
         "-disposition:v", "attached_pic", arg(tagged)});
    run({"ffmpeg", "-y", "-v", "error", "-i", arg(plain), "-i", arg(rectangle), "-map", "0:a", "-map", "1:v", "-c:a", "flac",
         "-c:v", "copy", "-disposition:v", "attached_pic", arg(flac)});
    const std::vector<fs::path> tracks{plain, tagged, flac};
    const auto songs = music::scan(tracks);
    check(!songs[0].has_embedded_artwork && songs[1].has_embedded_artwork && songs[2].has_embedded_artwork,
          "scan distinguishes tracks with attached artwork");
    check(music::embedded_artwork(plain).empty(), "track without attached art has no cover");
    const auto cover = music::embedded_artwork(tagged);
    check(!cover.empty() && square_png(cover), "MP3 embedded cover is extracted as a square PNG");
    check(!cover.empty() && dark(pixel(cover, 256, 95)) && red(pixel(cover, 256, 115)), "embedded art also uses proportional resizing");
    check(music::embedded_artwork(tagged) == cover, "embedded extraction reuses its source hash cache");
    const auto flacCover = music::embedded_artwork(flac);
    check(!flacCover.empty() && square_png(flacCover), "FLAC embedded cover is extracted");
    check(music::embedded_artwork(root / L"missing.mp3").empty(), "missing optional artwork is contained");
    const auto video = root / L"video.mp4";
    run({"ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i", "color=c=blue:s=64x64:d=1", "-i", arg(plain), "-c:v", "mpeg4",
         "-c:a", "aac", "-shortest", arg(video)});
    check(music::embedded_artwork(video).empty(), "ordinary video is not treated as embedded album art");
    if (!failures) std::cout << "artwork tests passed\n";
    return failures ? 1 : 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run_tests({argv, argv + argc}); }
#else
int main(int argc, char** argv) { return run_tests({argv, argv + argc}); }
#endif
