// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/ui/Canvas.hpp"

#include <string>

namespace akeno::ui {

std::vector<std::string> wrapText(ICanvas& canvas, std::string_view text, int width, FontRole role, bool bold) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view paragraph = text.substr(start, end - start);
        std::string line;
        std::size_t pos = 0;
        while (pos < paragraph.size()) {
            while (pos < paragraph.size() && paragraph[pos] == ' ') ++pos;
            if (pos >= paragraph.size()) break;
            std::size_t wordEnd = paragraph.find(' ', pos);
            if (wordEnd == std::string_view::npos) wordEnd = paragraph.size();
            std::string_view word = paragraph.substr(pos, wordEnd - pos);
            std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
            if (!line.empty() && canvas.measureText(candidate, role, bold).w > width) {
                lines.push_back(std::move(line));
                line = std::string(word);
            } else {
                line = std::move(candidate);
            }
            pos = wordEnd;
        }
        lines.push_back(std::move(line));  // an empty paragraph keeps its blank line
        if (end >= text.size()) break;
        start = end + 1;
    }
    return lines;
}

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
