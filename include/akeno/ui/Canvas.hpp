// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing interface used by all screens. The SDL backend implements it for the console and
// the desktop; tests use a recording implementation. Coordinates are logical 1920x1080.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace akeno::ui {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    Rect inset(int amount) const { return {x + amount, y + amount, w - 2 * amount, h - 2 * amount}; }
    bool operator==(const Rect&) const = default;
};

struct Size {
    int w = 0;
    int h = 0;
};

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    Color withAlpha(std::uint8_t alpha) const { return {r, g, b, alpha}; }
    bool operator==(const Color&) const = default;
};

enum class FontRole { Display, Title, Heading, Body, Caption, Small };
enum class TextAlign { Left, Center, Right };

struct TextStyle {
    FontRole role = FontRole::Body;
    Color color{242, 244, 248, 255};
    TextAlign align = TextAlign::Left;
    bool bold = false;
};

class ICanvas {
public:
    virtual ~ICanvas() = default;

    virtual void fillRect(const Rect& rect, Color color) = 0;
    virtual void fillRoundedRect(const Rect& rect, int radius, Color color) = 0;
    virtual void strokeRoundedRect(const Rect& rect, int radius, int thickness, Color color) = 0;
    virtual void fillCircle(int centerX, int centerY, int radius, Color color) = 0;

    // Draws one line of UTF-8 text inside `box`, vertically centred, shortened with "…" when it
    // does not fit the box width.
    virtual void drawText(std::string_view text, const Rect& box, const TextStyle& style) = 0;
    virtual Size measureText(std::string_view text, FontRole role, bool bold) = 0;

    // Draws a previously loaded image (see IImageService). Returns false when the image is not
    // ready, so the caller can draw a placeholder.
    virtual bool drawImage(const std::string& key, const Rect& rect) = 0;
};

// Breaks text into lines no wider than `width` (at spaces; '\n' starts a new line). A single
// word wider than the line is kept whole and shortened when drawn.
std::vector<std::string> wrapText(ICanvas& canvas, std::string_view text, int width, FontRole role, bool bold);

// Lays out text that may need several lines; returns the number of lines drawn.
int drawWrappedText(ICanvas& canvas, std::string_view text, const Rect& box, const TextStyle& style, int lineHeight,
                    int maxLines);

}  // namespace akeno::ui
