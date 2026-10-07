// SPDX-License-Identifier: GPL-3.0-or-later
#include "SdlCanvas.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "SdlImageService.hpp"
#include "akeno/core/Embedded.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui::sdl {

namespace {

constexpr std::size_t kMaxCachedTexts = 700;
constexpr std::size_t kMaxFitEntries = 2000;
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

std::size_t fontIndex(FontRole role, bool bold) { return static_cast<std::size_t>(role) * 2 + (bold ? 1 : 0); }

// Horizontal inset of a rounded corner at row `dy` (0 = outermost row) for radius `r`.
int cornerInset(int r, int dy) {
    const double fromCenter = r - dy - 0.5;
    const double span = std::sqrt(std::max(0.0, static_cast<double>(r) * r - fromCenter * fromCenter));
    return static_cast<int>(std::lround(r - span));
}

}  // namespace

SdlCanvas::SdlCanvas(SDL_Renderer* renderer, SdlImageService* images) : renderer_(renderer), images_(images) {}

SdlCanvas::~SdlCanvas() {
    for (auto& [key, cached] : textCache_) {
        if (cached.texture != nullptr) SDL_DestroyTexture(cached.texture);
    }
    for (TTF_Font* f : fonts_) {
        if (f != nullptr) TTF_CloseFont(f);
    }
}

Status SdlCanvas::loadFonts() {
    for (int roleIndex = 0; roleIndex < 6; ++roleIndex) {
        const auto role = static_cast<FontRole>(roleIndex);
        for (bool bold : {false, true}) {
            auto data = bold ? embedded::fontBold() : embedded::fontRegular();
            SDL_RWops* rw = SDL_RWFromConstMem(data.data(), static_cast<int>(data.size()));
            TTF_Font* loaded = rw != nullptr ? TTF_OpenFontRW(rw, 1, theme::fontSize(role)) : nullptr;
            if (loaded == nullptr) {
                return makeError(ErrorCode::Internal, "Could not load the user interface font.", TTF_GetError());
            }
            TTF_SetFontHinting(loaded, TTF_HINTING_LIGHT);
            fonts_[fontIndex(role, bold)] = loaded;
        }
    }
    return {};
}

void SdlCanvas::beginFrame() {
    ++frame_;
    if (images_ != nullptr) images_->beginFrame();
    evictText();
}

void SdlCanvas::setColor(Color color) {
    SDL_SetRenderDrawBlendMode(renderer_, color.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
}

void SdlCanvas::fillRect(const Rect& rect, Color color) {
    if (rect.w <= 0 || rect.h <= 0) return;
    setColor(color);
    SDL_Rect r{rect.x, rect.y, rect.w, rect.h};
    SDL_RenderFillRect(renderer_, &r);
}

void SdlCanvas::fillRoundedRect(const Rect& rect, int radius, Color color) {
    if (rect.w <= 0 || rect.h <= 0) return;
    radius = std::clamp(radius, 0, std::min(rect.w, rect.h) / 2);
    if (radius == 0) {
        fillRect(rect, color);
        return;
    }
    setColor(color);
    // One span per corner row plus the middle block: no pixel is drawn twice, so translucent
    // colours blend correctly.
    std::vector<SDL_Rect> spans;
    spans.reserve(static_cast<std::size_t>(radius) * 2 + 1);
    for (int dy = 0; dy < radius; ++dy) {
        const int inset = cornerInset(radius, dy);
        spans.push_back({rect.x + inset, rect.y + dy, rect.w - 2 * inset, 1});
        spans.push_back({rect.x + inset, rect.bottom() - 1 - dy, rect.w - 2 * inset, 1});
    }
    spans.push_back({rect.x, rect.y + radius, rect.w, rect.h - 2 * radius});
    SDL_RenderFillRects(renderer_, spans.data(), static_cast<int>(spans.size()));
}

void SdlCanvas::strokeRoundedRect(const Rect& rect, int radius, int thickness, Color color) {
    if (rect.w <= 0 || rect.h <= 0 || thickness <= 0) return;
    radius = std::clamp(radius, 0, std::min(rect.w, rect.h) / 2);
    thickness = std::min(thickness, std::min(rect.w, rect.h) / 2);
    setColor(color);
    const Rect inner{rect.x + thickness, rect.y + thickness, rect.w - 2 * thickness, rect.h - 2 * thickness};
    const int innerRadius = std::max(0, radius - thickness);
    std::vector<SDL_Rect> spans;
    spans.reserve(static_cast<std::size_t>(rect.h) * 2);
    for (int y = 0; y < rect.h; ++y) {
        int outerInset = 0;
        if (y < radius) outerInset = cornerInset(radius, y);
        else if (y >= rect.h - radius) outerInset = cornerInset(radius, rect.h - 1 - y);
        const int iy = y - thickness;
        if (iy < 0 || iy >= inner.h) {
            spans.push_back({rect.x + outerInset, rect.y + y, rect.w - 2 * outerInset, 1});
            continue;
        }
        int innerInset = 0;
        if (iy < innerRadius) innerInset = cornerInset(innerRadius, iy);
        else if (iy >= inner.h - innerRadius) innerInset = cornerInset(innerRadius, inner.h - 1 - iy);
        const int leftEnd = inner.x + innerInset;
        const int rightStart = inner.right() - innerInset;
        spans.push_back({rect.x + outerInset, rect.y + y, std::max(0, leftEnd - (rect.x + outerInset)), 1});
        spans.push_back({rightStart, rect.y + y, std::max(0, rect.right() - outerInset - rightStart), 1});
    }
    SDL_RenderFillRects(renderer_, spans.data(), static_cast<int>(spans.size()));
}

void SdlCanvas::fillCircle(int cx, int cy, int radius, Color color) {
    if (radius <= 0) return;
    fillRoundedRect({cx - radius, cy - radius, radius * 2, radius * 2}, radius, color);
}

TTF_Font* SdlCanvas::font(FontRole role, bool bold) { return fonts_[fontIndex(role, bold)]; }

Size SdlCanvas::measureText(std::string_view text, FontRole role, bool bold) {
    TTF_Font* f = font(role, bold);
    if (f == nullptr || text.empty()) return {0, theme::fontSize(role)};
    int w = 0;
    int h = 0;
    std::string copy(text);
    if (TTF_SizeUTF8(f, copy.c_str(), &w, &h) != 0) return {0, theme::fontSize(role)};
    return {w, h};
}

std::string SdlCanvas::fitText(std::string_view text, TTF_Font* f, int maxWidth) {
    std::string key = strings::concat(reinterpret_cast<std::uintptr_t>(f), "|", maxWidth, "|", text);
    if (auto it = fitCache_.find(key); it != fitCache_.end()) {
        return it->second;
    }
    std::string full(text);
    int w = 0;
    int h = 0;
    std::string result = full;
    if (TTF_SizeUTF8(f, full.c_str(), &w, &h) == 0 && w > maxWidth) {
        // Binary search over UTF-8 character boundaries for the longest prefix that fits
        // together with the ellipsis.
        std::vector<std::size_t> cuts;
        for (std::size_t pos = 0; pos <= full.size(); ++pos) {
            if (pos == full.size() || (static_cast<unsigned char>(full[pos]) & 0xC0) != 0x80) cuts.push_back(pos);
        }
        auto candidate = [&](std::size_t bytes) {
            return std::string(strings::trim(std::string_view(full).substr(0, bytes))) + std::string(kEllipsis);
        };
        std::size_t lo = 0;
        std::size_t hi = cuts.size() - 1;
        while (lo < hi) {
            const std::size_t mid = (lo + hi + 1) / 2;
            const std::string attempt = candidate(cuts[mid]);
            if (TTF_SizeUTF8(f, attempt.c_str(), &w, &h) == 0 && w <= maxWidth) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        result = lo == 0 ? std::string(kEllipsis) : candidate(cuts[lo]);
    }
    if (fitCache_.size() > kMaxFitEntries) fitCache_.clear();
    fitCache_.emplace(std::move(key), result);
    return result;
}

SdlCanvas::CachedText* SdlCanvas::textTexture(const std::string& text, FontRole role, bool bold, Color color) {
    std::string key = strings::concat(static_cast<int>(role), bold ? "b" : "r", "|", static_cast<int>(color.r), ",",
                                      static_cast<int>(color.g), ",", static_cast<int>(color.b), ",",
                                      static_cast<int>(color.a), "|", text);
    auto it = textCache_.find(key);
    if (it != textCache_.end()) {
        it->second.lastUsed = frame_;
        return &it->second;
    }
    TTF_Font* f = font(role, bold);
    if (f == nullptr) return nullptr;
    SDL_Surface* surface = TTF_RenderUTF8_Blended(f, text.c_str(), SDL_Color{color.r, color.g, color.b, 255});
    if (surface == nullptr) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
    CachedText cached{texture, surface->w, surface->h, frame_};
    SDL_FreeSurface(surface);
    if (texture == nullptr) return nullptr;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    if (color.a != 255) SDL_SetTextureAlphaMod(texture, color.a);
    auto [inserted, ok] = textCache_.emplace(std::move(key), cached);
    return &inserted->second;
}

void SdlCanvas::drawText(std::string_view text, const Rect& box, const TextStyle& style) {
    if (text.empty() || box.w <= 0) return;
    TTF_Font* f = font(style.role, style.bold);
    if (f == nullptr) return;
    const std::string shown = fitText(text, f, box.w);
    CachedText* cached = textTexture(shown, style.role, style.bold, style.color);
    if (cached == nullptr) return;
    int x = box.x;
    if (style.align == TextAlign::Center) x = box.x + (box.w - cached->w) / 2;
    if (style.align == TextAlign::Right) x = box.right() - cached->w;
    const int y = box.y + (box.h - cached->h) / 2;
    SDL_Rect destination{x, y, cached->w, cached->h};
    SDL_RenderCopy(renderer_, cached->texture, nullptr, &destination);
}

bool SdlCanvas::drawImage(const std::string& key, const Rect& rect) {
    if (images_ == nullptr || key.empty()) return false;
    SDL_Texture* texture = images_->texture(key);
    if (texture == nullptr) return false;
    SDL_Rect destination{rect.x, rect.y, rect.w, rect.h};
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_RenderCopy(renderer_, texture, nullptr, &destination);
    return true;
}

void SdlCanvas::evictText() {
    if (textCache_.size() <= kMaxCachedTexts) return;
    // Drop everything not drawn in the last few frames, oldest first.
    std::vector<std::pair<std::uint64_t, std::string>> candidates;
    for (const auto& [key, cached] : textCache_) {
        if (cached.lastUsed + 3 < frame_) candidates.emplace_back(cached.lastUsed, key);
    }
    std::sort(candidates.begin(), candidates.end());
    std::size_t toRemove = textCache_.size() - kMaxCachedTexts / 2;
    for (const auto& [used, key] : candidates) {
        if (toRemove == 0) break;
        auto it = textCache_.find(key);
        if (it != textCache_.end()) {
            SDL_DestroyTexture(it->second.texture);
            textCache_.erase(it);
            --toRemove;
        }
    }
}

}  // namespace akeno::ui::sdl
