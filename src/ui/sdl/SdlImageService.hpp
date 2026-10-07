// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <SDL2/SDL.h>

#include "akeno/core/Tasks.hpp"
#include "akeno/ui/AppController.hpp"

namespace akeno::ui::sdl {

// Fetches and decodes images on worker threads (validated and pre-scaled to their display
// size), then creates textures on the UI thread. Keeps a bounded LRU set of textures.
class SdlImageService final : public IImageLoader {
public:
    SdlImageService(SDL_Renderer* renderer, TaskRunner& tasks, MainThreadQueue& mainQueue, std::size_t maxTextures);
    ~SdlImageService() override;

    void ensure(const std::string& key, int size, std::function<Result<std::string>()> fetch) override;
    void retryFailed() override;

    // UI thread. nullptr until the image is ready.
    SDL_Texture* texture(const std::string& key);
    void beginFrame() { ++frame_; }
    // True once since the last call if a texture became ready (the screen needs a redraw).
    bool takeChanged();

private:
    enum class State { Loading, Ready, Failed };
    struct Entry {
        State state = State::Loading;
        SDL_Texture* texture = nullptr;
        std::uint64_t lastUsed = 0;
    };
    void evictIfNeeded();

    SDL_Renderer* renderer_;
    TaskRunner& tasks_;
    MainThreadQueue& mainQueue_;
    std::size_t maxTextures_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t frame_ = 0;
    bool changed_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace akeno::ui::sdl
