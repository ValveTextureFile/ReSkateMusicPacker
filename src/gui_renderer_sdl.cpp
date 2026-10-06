// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
// gui_renderer.h for macOS and Linux: ImGui through SDL2's renderer, images through stb_image.
#include "gui_renderer.h"

#include <SDL.h>
#include <backends/imgui_impl_sdlrenderer2.h>

#include "miniz.h"

#include <algorithm>
#include <cstdint>
#include <fstream>

#pragma GCC diagnostic push // third-party: silence warnings about the parts not used
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb/stb_image.h"
#pragma GCC diagnostic pop

namespace dingosdk::launcher_gui::detail {

bool Renderer::init(SDL_Window* window) {
    renderer_ = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) return false;
    return ImGui_ImplSDLRenderer2_Init(renderer_);
}

void Renderer::render() {
    const auto& io = ImGui::GetIO();
    // ImGui lays out in window points; draw at the display's pixel density (Retina, scaled Linux).
    SDL_RenderSetScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    const auto& background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    SDL_SetRenderDrawColor(renderer_, static_cast<Uint8>(background.x * 255), static_cast<Uint8>(background.y * 255),
                           static_cast<Uint8>(background.z * 255), 255);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (!capture_.empty()) {
        int width{}, height{};
        SDL_GetRendererOutputSize(renderer_, &width, &height);
        std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * height * 4);
        if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ABGR8888, rgba.data(), width * 4) == 0) {
            std::size_t length = 0;
            if (void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), width, height, 4, &length, 6, MZ_FALSE)) {
                std::ofstream(capture_, std::ios::binary).write(static_cast<const char*>(png), static_cast<std::streamsize>(length));
                mz_free(png);
            }
        }
        capture_.clear();
    }
    SDL_RenderPresent(renderer_);
}

ImTextureID Renderer::upload_texture(const std::vector<unsigned char>& pixels, unsigned width, unsigned height) {
    if (!renderer_ || !width || !height || pixels.size() < static_cast<std::size_t>(width) * height * 4) return {};
    auto* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                      static_cast<int>(width), static_cast<int>(height));
    if (!texture) return {};
    SDL_UpdateTexture(texture, nullptr, pixels.data(), static_cast<int>(width * 4));
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
}

void Renderer::release_texture(ImTextureID id) {
    if (id) SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(static_cast<std::uintptr_t>(id)));
}

void Renderer::shutdown() {
    if (!renderer_) return;
    ImGui_ImplSDLRenderer2_Shutdown();
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
}

bool decode_image(const std::vector<unsigned char>& bytes, ImVec2 cover, std::vector<unsigned char>& pixels,
                  unsigned& width, unsigned& height) {
    int w = 0, h = 0, channels = 0;
    auto* decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!decoded || w <= 0 || h <= 0) {
        stbi_image_free(decoded);
        return false;
    }
    width = static_cast<unsigned>(w);
    height = static_cast<unsigned>(h);
    const float fit = std::max(cover.x / static_cast<float>(w), cover.y / static_cast<float>(h));
    if (fit >= 1.0f) {
        pixels.assign(decoded, decoded + static_cast<std::size_t>(w) * h * 4);
        stbi_image_free(decoded);
        return true;
    }
    // Box filter: each target pixel averages the source pixels it covers.
    width = std::max(1u, static_cast<unsigned>(static_cast<float>(w) * fit + 0.5f));
    height = std::max(1u, static_cast<unsigned>(static_cast<float>(h) * fit + 0.5f));
    pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned y0 = y * static_cast<unsigned>(h) / height, y1 = std::max(y0 + 1, (y + 1) * static_cast<unsigned>(h) / height);
        for (unsigned x = 0; x < width; ++x) {
            const unsigned x0 = x * static_cast<unsigned>(w) / width, x1 = std::max(x0 + 1, (x + 1) * static_cast<unsigned>(w) / width);
            std::uint32_t sum[4]{};
            for (unsigned sy = y0; sy < y1; ++sy)
                for (unsigned sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += decoded[(static_cast<std::size_t>(sy) * w + sx) * 4 + c];
            const auto count = (y1 - y0) * (x1 - x0);
            for (int c = 0; c < 4; ++c)
                pixels[(static_cast<std::size_t>(y) * width + x) * 4 + c] = static_cast<unsigned char>(sum[c] / count);
        }
    }
    stbi_image_free(decoded);
    return true;
}

} // namespace dingosdk::launcher_gui::detail
