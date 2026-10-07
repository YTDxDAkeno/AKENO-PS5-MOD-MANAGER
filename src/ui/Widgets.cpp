// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/Widgets.hpp"

#include <algorithm>
#include <cmath>

namespace akeno::ui {

// ---------------------------------------------------------------- FocusList

void FocusList::setCount(int count) {
    count_ = std::max(0, count);
    focus_ = count_ == 0 ? 0 : std::clamp(focus_, 0, count_ - 1);
    clampScroll();
}

void FocusList::setVisibleRows(int rows) {
    visibleRows_ = std::max(1, rows);
    clampScroll();
}

void FocusList::setFocus(int index) {
    focus_ = count_ == 0 ? 0 : std::clamp(index, 0, count_ - 1);
    clampScroll();
}

bool FocusList::handle(Action action) {
    if (count_ == 0) {
        return false;
    }
    int previous = focus_;
    switch (action) {
        case Action::Up: focus_ = std::max(0, focus_ - 1); break;
        case Action::Down: focus_ = std::min(count_ - 1, focus_ + 1); break;
        case Action::PageUp: focus_ = std::max(0, focus_ - visibleRows_); break;
        case Action::PageDown: focus_ = std::min(count_ - 1, focus_ + visibleRows_); break;
        default: return false;
    }
    clampScroll();
    return focus_ != previous;
}

void FocusList::clampScroll() {
    if (focus_ < first_) first_ = focus_;
    if (focus_ >= first_ + visibleRows_) first_ = focus_ - visibleRows_ + 1;
    first_ = std::clamp(first_, 0, std::max(0, count_ - visibleRows_));
}

// ---------------------------------------------------------------- FocusGrid

void FocusGrid::setCount(int count) {
    count_ = std::max(0, count);
    focus_ = count_ == 0 ? 0 : std::clamp(focus_, 0, count_ - 1);
    clampScroll();
}

void FocusGrid::setColumns(int columns) {
    columns_ = std::max(1, columns);
    clampScroll();
}

void FocusGrid::setVisibleRows(int rows) {
    visibleRows_ = std::max(1, rows);
    clampScroll();
}

void FocusGrid::setFocus(int index) {
    focus_ = count_ == 0 ? 0 : std::clamp(index, 0, count_ - 1);
    clampScroll();
}

bool FocusGrid::handle(Action action) {
    if (count_ == 0) {
        return false;
    }
    int previous = focus_;
    const int column = focus_ % columns_;
    switch (action) {
        case Action::Left:
            if (column > 0) --focus_;
            break;
        case Action::Right:
            if (column < columns_ - 1 && focus_ + 1 < count_) ++focus_;
            break;
        case Action::Up:
            if (focus_ - columns_ >= 0) focus_ -= columns_;
            break;
        case Action::Down:
            if (focus_ + columns_ < count_) {
                focus_ += columns_;
            } else if (focus_ / columns_ < rowCount() - 1) {
                focus_ = count_ - 1;  // last row is shorter: land on its last item
            }
            break;
        case Action::PageUp:
            focus_ = std::max(column, focus_ - columns_ * visibleRows_);
            break;
        case Action::PageDown:
            focus_ = std::min(count_ - 1, focus_ + columns_ * visibleRows_);
            break;
        default:
            return false;
    }
    clampScroll();
    return focus_ != previous;
}

void FocusGrid::clampScroll() {
    const int row = columns_ > 0 ? focus_ / columns_ : 0;
    if (row < firstRow_) firstRow_ = row;
    if (row >= firstRow_ + visibleRows_) firstRow_ = row - visibleRows_ + 1;
    firstRow_ = std::clamp(firstRow_, 0, std::max(0, rowCount() - visibleRows_));
}

// ---------------------------------------------------------------- drawing helpers

namespace draw {

void focusRing(ICanvas& canvas, const Rect& rect, int radius) {
    canvas.strokeRoundedRect({rect.x - 8, rect.y - 8, rect.w + 16, rect.h + 16}, radius + 8, 3,
                             theme::kAccent.withAlpha(140));
    canvas.strokeRoundedRect({rect.x - 4, rect.y - 4, rect.w + 8, rect.h + 8}, radius + 4, 4, theme::kFocus);
}

void panel(ICanvas& canvas, const Rect& rect, bool raised) {
    canvas.fillRoundedRect(rect, 16, raised ? theme::kPanelRaised : theme::kPanel);
}

void button(ICanvas& canvas, const Rect& rect, std::string_view label, bool focused, bool enabled,
            std::string_view value) {
    Color background = focused ? theme::kPanelRaised : theme::kPanel;
    canvas.fillRoundedRect(rect, 12, background);
    if (focused) {
        focusRing(canvas, rect, 12);
    }
    Color textColor = enabled ? theme::kTextPrimary : theme::kTextDisabled;
    // The value is drawn bold, so it must be measured bold too (plus a little slack for rounding).
    const int valueWidth =
        value.empty() ? 0 : std::min(rect.w / 2, canvas.measureText(value, FontRole::Body, true).w + 4);
    canvas.drawText(label, {rect.x + 28, rect.y, rect.w - 56 - valueWidth - (valueWidth > 0 ? 24 : 0), rect.h},
                    TextStyle{FontRole::Body, textColor, TextAlign::Left, focused});
    if (!value.empty()) {
        canvas.drawText(value, {rect.right() - 28 - valueWidth, rect.y, valueWidth, rect.h},
                        TextStyle{FontRole::Body, enabled ? theme::kAccent : theme::kTextDisabled, TextAlign::Right,
                                  true});
    }
}

int badge(ICanvas& canvas, int x, int y, std::string_view label, Color color, FontRole role) {
    Size size = canvas.measureText(label, role, true);
    const int height = theme::fontSize(role) + 16;
    const int width = size.w + 28;
    canvas.fillRoundedRect({x, y, width, height}, height / 2, color);
    canvas.drawText(label, {x, y, width, height}, TextStyle{role, theme::kTextPrimary, TextAlign::Center, true});
    return width;
}

void buttonGlyph(ICanvas& canvas, int cx, int cy, ButtonHint::Button button) {
    const int r = 20;
    switch (button) {
        case ButtonHint::Button::Cross:
            canvas.fillCircle(cx, cy, r, theme::kPanelRaised);
            canvas.drawText("\xE2\x9C\x95", {cx - r, cy - r, 2 * r, 2 * r},
                            TextStyle{FontRole::Small, theme::kButtonCross, TextAlign::Center, true});
            break;
        case ButtonHint::Button::Circle:
            canvas.fillCircle(cx, cy, r, theme::kPanelRaised);
            canvas.drawText("\xE2\x97\x8B", {cx - r, cy - r, 2 * r, 2 * r},
                            TextStyle{FontRole::Small, theme::kButtonCircle, TextAlign::Center, true});
            break;
        case ButtonHint::Button::Triangle:
            canvas.fillCircle(cx, cy, r, theme::kPanelRaised);
            canvas.drawText("\xE2\x96\xB3", {cx - r, cy - r, 2 * r, 2 * r},
                            TextStyle{FontRole::Small, theme::kButtonTriangle, TextAlign::Center, true});
            break;
        case ButtonHint::Button::Square:
            canvas.fillCircle(cx, cy, r, theme::kPanelRaised);
            canvas.drawText("\xE2\x96\xA1", {cx - r, cy - r, 2 * r, 2 * r},
                            TextStyle{FontRole::Small, theme::kButtonSquare, TextAlign::Center, true});
            break;
        case ButtonHint::Button::Options:
            canvas.fillRoundedRect({cx - 34, cy - 16, 68, 32}, 16, theme::kPanelRaised);
            canvas.drawText("OPT", {cx - 34, cy - 16, 68, 32},
                            TextStyle{FontRole::Small, theme::kTextPrimary, TextAlign::Center, true});
            break;
        case ButtonHint::Button::L1R1:
            canvas.fillRoundedRect({cx - 44, cy - 16, 88, 32}, 8, theme::kPanelRaised);
            canvas.drawText("L1 R1", {cx - 44, cy - 16, 88, 32},
                            TextStyle{FontRole::Small, theme::kTextPrimary, TextAlign::Center, true});
            break;
        case ButtonHint::Button::L2R2:
            canvas.fillRoundedRect({cx - 44, cy - 16, 88, 32}, 8, theme::kPanelRaised);
            canvas.drawText("L2 R2", {cx - 44, cy - 16, 88, 32},
                            TextStyle{FontRole::Small, theme::kTextPrimary, TextAlign::Center, true});
            break;
        case ButtonHint::Button::LeftRight:
            canvas.fillRoundedRect({cx - 44, cy - 16, 88, 32}, 8, theme::kPanelRaised);
            canvas.drawText("\xE2\x97\x80 \xE2\x96\xB6", {cx - 44, cy - 16, 88, 32},
                            TextStyle{FontRole::Small, theme::kTextPrimary, TextAlign::Center, true});
            break;
    }
}

void footerHints(ICanvas& canvas, const std::vector<ButtonHint>& hints) {
    const Rect footer = theme::kFooter;
    canvas.fillRect(footer, theme::kBackgroundTop);
    int x = footer.right() - theme::kMargin;
    // Right-aligned, last hint furthest right.
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        const int labelWidth = canvas.measureText(it->label, FontRole::Caption, false).w;
        const bool wideGlyph = it->button == ButtonHint::Button::Options || it->button == ButtonHint::Button::L1R1 ||
                               it->button == ButtonHint::Button::L2R2 || it->button == ButtonHint::Button::LeftRight;
        const int glyphWidth = wideGlyph ? 92 : 44;
        x -= labelWidth;
        canvas.drawText(it->label, {x, footer.y, labelWidth, footer.h},
                        TextStyle{FontRole::Caption, theme::kTextSecondary, TextAlign::Left, false});
        x -= 12 + glyphWidth / 2;
        buttonGlyph(canvas, x, footer.y + footer.h / 2, it->button);
        x -= glyphWidth / 2 + 40;
    }
}

void spinner(ICanvas& canvas, int cx, int cy, double time) {
    constexpr int kDots = 8;
    constexpr double kPi = 3.14159265358979323846;
    const int active = static_cast<int>(time * 10.0) % kDots;
    for (int i = 0; i < kDots; ++i) {
        double angle = 2.0 * kPi * i / kDots;
        int x = cx + static_cast<int>(std::lround(std::cos(angle) * 28.0));
        int y = cy + static_cast<int>(std::lround(std::sin(angle) * 28.0));
        int distance = (active - i + kDots) % kDots;
        auto alpha = static_cast<std::uint8_t>(255 - distance * 28);
        canvas.fillCircle(x, y, 7, theme::kAccent.withAlpha(alpha));
    }
}

void placeholderArt(ICanvas& canvas, const Rect& rect, std::string_view name) {
    unsigned hash = 2166136261u;
    for (char c : name) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    const Color tint{static_cast<std::uint8_t>(40 + (hash & 0x3F)), static_cast<std::uint8_t>(50 + ((hash >> 8) & 0x3F)),
                     static_cast<std::uint8_t>(90 + ((hash >> 16) & 0x5F)), 255};
    canvas.fillRoundedRect(rect, 14, tint);
    std::string initials;
    bool takeNext = true;
    for (char c : name) {
        if (c == ' ') {
            takeNext = true;
        } else if (takeNext && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            initials.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
            takeNext = false;
            if (initials.size() == 2) break;
        } else {
            takeNext = false;
        }
    }
    if (initials.empty()) initials = "?";
    canvas.drawText(initials, rect, TextStyle{FontRole::Display, theme::kTextPrimary.withAlpha(220), TextAlign::Center, true});
}

void progressBar(ICanvas& canvas, const Rect& rect, double fraction, Color color) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    canvas.fillRoundedRect(rect, rect.h / 2, theme::kPanelRaised);
    const int filled = static_cast<int>(rect.w * fraction);
    if (filled > rect.h) {
        canvas.fillRoundedRect({rect.x, rect.y, filled, rect.h}, rect.h / 2, color);
    }
}

void messagePanel(ICanvas& canvas, const Rect& rect, std::string_view heading, std::string_view body, Color accent) {
    panel(canvas, rect);
    canvas.fillRoundedRect({rect.x, rect.y, 10, rect.h}, 5, accent);
    canvas.drawText(heading, {rect.x + 50, rect.y + 36, rect.w - 100, 56},
                    TextStyle{FontRole::Heading, theme::kTextPrimary, TextAlign::Left, true});
    drawWrappedText(canvas, body, {rect.x + 50, rect.y + 110, rect.w - 100, rect.h - 130},
                    TextStyle{FontRole::Body, theme::kTextSecondary, TextAlign::Left, false}, 46,
                    std::max(1, (rect.h - 130) / 46));
}

void addWrappedLines(ICanvas& canvas, std::vector<DocLine>& lines, std::string_view text, int width, FontRole role,
                     Color color, bool bold, std::optional<Color> dot) {
    bool first = true;
    for (auto& line : wrapText(canvas, text, width - (dot ? 40 : 0), role, bold)) {
        lines.push_back(DocLine{std::move(line), role, color, bold, first ? dot : std::nullopt});
        first = false;
    }
}

void addHeadingLine(std::vector<DocLine>& lines, std::string text) {
    if (!lines.empty()) lines.push_back(DocLine{});
    lines.push_back(DocLine{std::move(text), FontRole::Body, theme::kTextPrimary, true, std::nullopt});
}

void drawDocument(ICanvas& canvas, const std::vector<DocLine>& lines, const Rect& area, int& scroll, int& maxScroll) {
    const int visible = std::max(1, area.h / kDocLineHeight);
    maxScroll = std::max(0, static_cast<int>(lines.size()) - visible);
    scroll = std::clamp(scroll, 0, maxScroll);
    for (int i = 0; i < visible && scroll + i < static_cast<int>(lines.size()); ++i) {
        const DocLine& line = lines[static_cast<std::size_t>(scroll + i)];
        const int y = area.y + i * kDocLineHeight;
        int x = area.x;
        if (line.dot) {
            canvas.fillCircle(x + 12, y + kDocLineHeight / 2, 10, *line.dot);
            x += 40;
        }
        canvas.drawText(line.text, {x, y, area.right() - x - 30, kDocLineHeight},
                        TextStyle{line.role, line.color, TextAlign::Left, line.bold});
    }
    if (maxScroll > 0) {
        const Rect track{area.right() - 10, area.y, 8, visible * kDocLineHeight};
        canvas.fillRoundedRect(track, 4, theme::kPanelRaised);
        const int thumbH = std::max(40, track.h * visible / static_cast<int>(lines.size()));
        const int thumbY = track.y + (track.h - thumbH) * scroll / maxScroll;
        canvas.fillRoundedRect({track.x, thumbY, track.w, thumbH}, 4, theme::kAccent);
    }
}

}  // namespace draw

}  // namespace akeno::ui
