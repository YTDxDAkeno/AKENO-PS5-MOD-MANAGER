// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "akeno/core/Result.hpp"
#include "akeno/ui/Canvas.hpp"

namespace akeno::ui::sdl {

class SdlImageService;

// ICanvas on an SDL_Renderer (software renderer on the console). Text is rendered with
// SDL_ttf from the embedded DejaVu fonts and cached as textures.
class SdlCanvas final : public ICanvas {
public:
    SdlCanvas(SDL_Renderer* renderer, SdlImageService* images);
    ~SdlCanvas() override;
    SdlCanvas(const SdlCanvas&) = delete;
    SdlCanvas& operator=(const SdlCanvas&) = delete;

    Status loadFonts();
    void beginFrame();

    void fillRect(const Rect& rect, Color color) override;
    void fillRoundedRect(const Rect& rect, int radius, Color color) override;
    void strokeRoundedRect(const Rect& rect, int radius, int thickness, Color color) override;
    void fillCircle(int centerX, int centerY, int radius, Color color) override;
    void drawText(std::string_view text, const Rect& box, const TextStyle& style) override;
    Size measureText(std::string_view text, FontRole role, bool bold) override;
    bool drawImage(const std::string& key, const Rect& rect) override;

private:
    struct CachedText {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
        std::uint64_t lastUsed = 0;
    };
    TTF_Font* font(FontRole role, bool bold);
    std::string fitText(std::string_view text, TTF_Font* font, int maxWidth);
    CachedText* textTexture(const std::string& text, FontRole role, bool bold, Color color);
    void setColor(Color color);
    void evictText();

    SDL_Renderer* renderer_;
    SdlImageService* images_;
    std::array<TTF_Font*, 12> fonts_{};
    std::unordered_map<std::string, CachedText> textCache_;
    std::unordered_map<std::string, std::string> fitCache_;
    std::uint64_t frame_ = 0;
};

}  // namespace akeno::ui::sdl
