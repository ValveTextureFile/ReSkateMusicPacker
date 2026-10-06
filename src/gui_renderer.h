// SPDX-FileCopyrightText: 2026 DeckardDetribine and the ReSkateMusicPacker contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef _WIN32
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <imgui.h>

#include <array>
#include <filesystem>
#include <vector>

// The launcher window's Direct3D 12 renderer and its image decoding.
namespace dingosdk::launcher_gui::detail {

// The window handle the renderer, file dialogs and GUI pages take.
using NativeWindow = HWND;

using Microsoft::WRL::ComPtr;

constexpr UINT frame_count = 2;

class Renderer {
public:
    bool init(HWND window);
    void render();
    // Uploads RGBA pixels into a free SRV slot (slot 0 is the font) and
    // returns its ImGui texture id; empty when the slots are used up.
    ImTextureID upload_texture(const std::vector<unsigned char>& pixels, UINT width, UINT height);
    // Frees a texture's slot once the GPU is done with it (Mods browser icons).
    void release_texture(ImTextureID id);
    void shutdown();

private:
    struct Frame { ComPtr<ID3D12CommandAllocator> allocator; UINT64 fence_value{}; };
    struct Target { ComPtr<ID3D12Resource> resource; D3D12_CPU_DESCRIPTOR_HANDLE handle{}; };
    ComPtr<ID3D12Device> device_;
    // The background photo, tile icons and Thunderstore package icons.
    static constexpr UINT max_textures = 160;
    std::vector<ComPtr<ID3D12Resource>> textures_;   // index + 1 = SRV slot; null when free
    struct RetiredTexture {
        UINT64 fence_value{};
        std::size_t slot_index{};
        ComPtr<ID3D12Resource> resource;
    };
    std::vector<RetiredTexture> retired_textures_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<IDXGISwapChain3> swap_;
    std::array<Frame, frame_count> frames_;
    std::array<Target, frame_count> targets_;
    HANDLE fence_event_{};
    HANDLE waitable_{};
    UINT64 fence_value_{};
    UINT64 frame_index_{};

    void wait(UINT64 value);
};

// Decodes a JPEG/PNG with WIC, scaled down to cover `cover` pixels at most.
bool decode_image(const std::vector<unsigned char>& bytes, ImVec2 cover, std::vector<unsigned char>& pixels,
                  UINT& width, UINT& height);
// ReSkateLauncher.background.jpg/.png beside the exe, else the embedded photo.
std::vector<unsigned char> background_bytes(const std::filesystem::path& directory);
// An embedded RCDATA resource; empty when it is missing.
std::vector<unsigned char> resource_bytes(const wchar_t* name);

} // namespace dingosdk::launcher_gui::detail
#else // macOS and Linux: SDL2 and its 2D renderer (Metal, OpenGL or software, whichever works).

#include <imgui.h>

#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Window;

namespace dingosdk::launcher_gui::detail {

using NativeWindow = SDL_Window*;

class Renderer {
public:
    bool init(SDL_Window* window);
    void render();
    // Uploads RGBA pixels and returns the texture's ImGui id; empty on failure.
    ImTextureID upload_texture(const std::vector<unsigned char>& pixels, unsigned width, unsigned height);
    void release_texture(ImTextureID id);
    void shutdown();
    SDL_Renderer* sdl() const { return renderer_; }
    // Saves the next rendered frame as a PNG (RSMP_CAPTURE, for checking the UI without a screen).
    void capture_next(std::string path) { capture_ = std::move(path); }

private:
    SDL_Renderer* renderer_{};
    std::string capture_;
};

// Decodes a JPEG/PNG to RGBA with stb_image, scaled down to cover `cover` pixels at most.
bool decode_image(const std::vector<unsigned char>& bytes, ImVec2 cover, std::vector<unsigned char>& pixels,
                  unsigned& width, unsigned& height);

} // namespace dingosdk::launcher_gui::detail
#endif
