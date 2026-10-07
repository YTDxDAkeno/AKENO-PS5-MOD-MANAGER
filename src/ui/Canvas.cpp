// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/Canvas.hpp"

#include <string>

namespace akeno::ui {

int drawWrappedText(ICanvas& canvas, std::string_view text, const Rect& box, const TextStyle& style, int lineHeight,
                    int maxLines) {
    if (maxLines <= 0 || text.empty()) {
        return 0;
    }
    int lines = 0;
    std::size_t pos = 0;
    while (pos < text.size() && lines < maxLines) {
        while (pos < text.size() && text[pos] == ' ') ++pos;
        if (pos >= text.size()) break;
        const Rect lineBox{box.x, box.y + lines * lineHeight, box.w, lineHeight};
        if (lines == maxLines - 1) {
            // Last allowed line: let drawText shorten whatever remains.
            canvas.drawText(text.substr(pos), lineBox, style);
            return lines + 1;
        }
        std::size_t lineEnd = pos;
        std::size_t scan = pos;
        while (scan <= text.size()) {
            std::size_t next = text.find(' ', scan);
            if (next == std::string_view::npos) next = text.size();
            std::string_view candidate = text.substr(pos, next - pos);
            if (canvas.measureText(candidate, style.role, style.bold).w > box.w && lineEnd > pos) {
                break;
            }
            lineEnd = next;
            if (next >= text.size()) break;
            scan = next + 1;
        }
        if (lineEnd == pos) {
            lineEnd = text.find(' ', pos);
            if (lineEnd == std::string_view::npos) lineEnd = text.size();
        }
        canvas.drawText(text.substr(pos, lineEnd - pos), lineBox, style);
        ++lines;
        pos = lineEnd;
    }
    return lines;
}

}  // namespace akeno::ui
