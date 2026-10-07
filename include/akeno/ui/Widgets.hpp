// SPDX-License-Identifier: GPL-3.0-or-later
// Focus/scroll models and shared drawing helpers.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "akeno/ui/Canvas.hpp"
#include "akeno/ui/Input.hpp"
#include "akeno/ui/Theme.hpp"

namespace akeno::ui {

// Vertical list with a focused index and a scroll window of `visibleRows` rows.
class FocusList {
public:
    void setCount(int count);
    void setVisibleRows(int rows);
    // Returns true if the action moved the focus.
    bool handle(Action action);
    void setFocus(int index);

    int count() const { return count_; }
    int focus() const { return focus_; }
    int firstVisible() const { return first_; }
    int visibleRows() const { return visibleRows_; }

private:
    void clampScroll();
    int count_ = 0;
    int focus_ = 0;
    int first_ = 0;
    int visibleRows_ = 1;
};

// Grid with `columns` columns; scrolls by whole rows.
class FocusGrid {
public:
    void setCount(int count);
    void setColumns(int columns);
    void setVisibleRows(int rows);
    bool handle(Action action);
    void setFocus(int index);

    int count() const { return count_; }
    int focus() const { return focus_; }
    int columns() const { return columns_; }
    int firstVisibleRow() const { return firstRow_; }
    int visibleRows() const { return visibleRows_; }
    int rowCount() const { return columns_ > 0 ? (count_ + columns_ - 1) / columns_ : 0; }

private:
    void clampScroll();
    int count_ = 0;
    int focus_ = 0;
    int columns_ = 1;
    int visibleRows_ = 1;
    int firstRow_ = 0;
};

struct ButtonHint {
    enum class Button { Cross, Circle, Triangle, Square, Options, L1R1, L2R2 };
    Button button;
    std::string label;
};

namespace draw {

void focusRing(ICanvas& canvas, const Rect& rect, int radius = 14);
void panel(ICanvas& canvas, const Rect& rect, bool raised = false);
// A selectable row/button. Disabled entries are drawn dimmed.
void button(ICanvas& canvas, const Rect& rect, std::string_view label, bool focused, bool enabled = true,
            std::string_view value = {});
// Rounded label such as "VERIFIED" or "SAFE MODE"; returns its width.
int badge(ICanvas& canvas, int x, int y, std::string_view label, Color color, FontRole role = FontRole::Small);
void buttonGlyph(ICanvas& canvas, int centerX, int centerY, ButtonHint::Button button);
void footerHints(ICanvas& canvas, const std::vector<ButtonHint>& hints);
// Animated "working" indicator; `time` in seconds.
void spinner(ICanvas& canvas, int centerX, int centerY, double time);
// Coloured initials tile used when a game has no icon yet.
void placeholderArt(ICanvas& canvas, const Rect& rect, std::string_view name);
void progressBar(ICanvas& canvas, const Rect& rect, double fraction, Color color);

}  // namespace draw

}  // namespace akeno::ui
