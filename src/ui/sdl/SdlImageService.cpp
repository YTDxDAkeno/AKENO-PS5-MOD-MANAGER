// SPDX-License-Identifier: GPL-3.0-or-later
#include "SdlImageService.hpp"

#include <algorithm>
#include <vector>

#include <SDL2/SDL_image.h>

#include "akeno/logging/Logger.hpp"
#include "akeno/security/ImageProbe.hpp"

namespace akeno::ui::sdl {

using logging::logger;

namespace {

// Decodes `bytes` and scales the result into a width x height ARGB surface, letterboxing
// images with a different aspect ratio. Runs on a worker thread.
SDL_Surface* decodeScaled(const std::string& bytes, int width, int height, std::string& error) {
    auto info = security::validateImage(bytes);
    if (!info) {
        error = info.error().describe();
        return nullptr;
    }
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
    if (rw == nullptr) {
        error = SDL_GetError();
        return nullptr;
    }
    SDL_Surface* decoded = IMG_Load_RW(rw, 1);
    if (decoded == nullptr) {
        error = IMG_GetError();
        return nullptr;
    }
    // The decoder must agree with the header we validated.
    if (decoded->w != static_cast<int>(info->width) || decoded->h != static_cast<int>(info->height)) {
        SDL_FreeSurface(decoded);
        error = "decoded size differs from the image header";
        return nullptr;
    }
    SDL_Surface* target = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (target == nullptr) {
        SDL_FreeSurface(decoded);
        error = SDL_GetError();
        return nullptr;
    }
    SDL_FillRect(target, nullptr, SDL_MapRGBA(target->format, 0, 0, 0, 0));
    const double scale = std::min(static_cast<double>(width) / decoded->w, static_cast<double>(height) / decoded->h);
    SDL_Rect destination{0, 0, std::max(1, static_cast<int>(decoded->w * scale)),
                         std::max(1, static_cast<int>(decoded->h * scale))};
    destination.x = (width - destination.w) / 2;
    destination.y = (height - destination.h) / 2;
    SDL_SetSurfaceBlendMode(decoded, SDL_BLENDMODE_NONE);
    if (SDL_BlitScaled(decoded, nullptr, target, &destination) != 0) {
        error = SDL_GetError();
        SDL_FreeSurface(decoded);
        SDL_FreeSurface(target);
        return nullptr;
    }
    SDL_FreeSurface(decoded);
    return target;
}

}  // namespace

SdlImageService::SdlImageService(SDL_Renderer* renderer, TaskRunner& tasks, MainThreadQueue& mainQueue,
                                 std::size_t maxTextures, std::uint64_t maxPixels)
    : renderer_(renderer),
      tasks_(tasks),
      mainQueue_(mainQueue),
      maxTextures_(std::max<std::size_t>(8, maxTextures)),
      maxPixels_(std::max<std::uint64_t>(4ull * 1024 * 1024, maxPixels)) {}

SdlImageService::~SdlImageService() {
    alive_->store(false);
    for (auto& [key, entry] : entries_) {
        if (entry.texture != nullptr) {
            SDL_DestroyTexture(entry.texture);
        }
    }
}

void SdlImageService::ensure(const std::string& key, int width, int height,
                             std::function<Result<std::string>()> fetch) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.lastUsed = frame_;
        return;
    }
    entries_[key] = Entry{State::Loading, nullptr, frame_};
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    MainThreadQueue* queue = &mainQueue_;
    tasks_.submit([this, queue, key, width, height, fetch = std::move(fetch), alive] {
        auto bytes = fetch();
        std::string error;
        SDL_Surface* surface = nullptr;
        if (bytes) {
            surface = decodeScaled(bytes.value(), width, height, error);
        } else {
            error = bytes.error().describe();
        }
        queue->post([this, key, surface, error, alive] {
            auto guard = alive.lock();
            if (!guard || !guard->load()) {
                if (surface != nullptr) SDL_FreeSurface(surface);
                return;
            }
            auto entry = entries_.find(key);
            if (entry == entries_.end()) {
                if (surface != nullptr) SDL_FreeSurface(surface);
                return;
            }
            if (surface == nullptr) {
                entry->second.state = State::Failed;
                if (!error.empty()) logger().debug("images", key + ": " + error);
                return;
            }
            entry->second.texture = SDL_CreateTextureFromSurface(renderer_, surface);
            entry->second.pixels = static_cast<std::uint64_t>(surface->w) * static_cast<std::uint64_t>(surface->h);
            SDL_FreeSurface(surface);
            entry->second.state = entry->second.texture != nullptr ? State::Ready : State::Failed;
            changed_ = true;
            evictIfNeeded();
        });
    });
}

void SdlImageService::retryFailed() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        it = it->second.state == State::Failed ? entries_.erase(it) : std::next(it);
    }
}

SDL_Texture* SdlImageService::texture(const std::string& key) {
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.state != State::Ready) {
        return nullptr;
    }
    it->second.lastUsed = frame_;
    return it->second.texture;
}

bool SdlImageService::takeChanged() {
    bool changed = changed_;
    changed_ = false;
    return changed;
}

void SdlImageService::evictIfNeeded() {
    std::size_t ready = 0;
    std::uint64_t pixels = 0;
    for (const auto& [key, entry] : entries_) {
        if (entry.state == State::Ready) {
            ++ready;
            pixels += entry.pixels;
        }
    }
    while (ready > maxTextures_ || pixels > maxPixels_) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.state != State::Ready || it->second.lastUsed + 1 >= frame_) continue;  // in use now
            if (oldest == entries_.end() || it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        }
        if (oldest == entries_.end()) break;
        SDL_DestroyTexture(oldest->second.texture);
        pixels -= oldest->second.pixels;
        entries_.erase(oldest);
        --ready;
    }
}

}  // namespace akeno::ui::sdl
