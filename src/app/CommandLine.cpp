// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/app/CommandLine.hpp"

#include <charconv>
#include <string_view>

namespace akeno::app {

namespace {

bool parseInt(std::string_view text, int minimum, int maximum, int& out) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size() || value < minimum || value > maximum) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

CommandLine parseCommandLine(int argc, const char* const* argv) {
    CommandLine cl;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i] != nullptr ? argv[i] : "";
        auto needValue = [&](std::string_view name) -> std::optional<std::string_view> {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                cl.errors.push_back(std::string(name) + " needs a value");
                return std::nullopt;
            }
            return std::string_view(argv[++i]);
        };
        if (arg == "--self-check") {
            cl.mode = RunMode::SelfCheck;
        } else if (arg == "--list-games") {
            cl.mode = RunMode::ListGames;
        } else if (arg == "--help" || arg == "-h") {
            cl.mode = RunMode::Help;
        } else if (arg == "--version") {
            cl.mode = RunMode::Version;
        } else if (arg == "--verbose") {
            cl.verbose = true;
        } else if (arg == "--fullscreen") {
            cl.fullscreen = true;
        } else if (arg == "--data-root") {
            if (auto value = needValue(arg)) {
                if (value->empty() || value->front() != '/') {
                    cl.errors.push_back("--data-root must be an absolute path");
                } else {
                    cl.dataRoot = std::filesystem::path(std::string(*value));
                }
            }
        } else if (arg == "--shadowmount-port") {
            if (auto value = needValue(arg)) {
                int port = 0;
                if (parseInt(*value, 1, 65535, port)) {
                    cl.shadowMountPort = port;
                } else {
                    cl.errors.push_back("--shadowmount-port must be between 1 and 65535");
                }
            }
        } else if (arg == "--window") {
            if (auto value = needValue(arg)) {
                std::size_t x = value->find('x');
                int w = 0;
                int h = 0;
                if (x == std::string_view::npos || !parseInt(value->substr(0, x), 320, 7680, w) ||
                    !parseInt(value->substr(x + 1), 240, 4320, h)) {
                    cl.errors.push_back("--window must look like 1280x720");
                } else {
                    cl.windowWidth = w;
                    cl.windowHeight = h;
                }
            }
        } else {
            cl.errors.push_back("unknown option: " + std::string(arg));
        }
    }
    return cl;
}

std::string usageText() {
    return "Akeno PS5 Mod Manager\n"
           "\n"
           "Usage: AkenoModManager [options]\n"
           "\n"
           "  (no option)              start the controller user interface\n"
           "  --self-check             run the system check, write a report to logs/ and exit\n"
           "  --list-games             list games reported by ShadowMountPlus and exit\n"
           "  --data-root <dir>        application data directory\n"
           "                           (console default: /data/akeno-mod-manager)\n"
           "  --shadowmount-port <n>   ShadowMountPlus API port on 127.0.0.1 (default 10101)\n"
           "  --window <WxH>           window size for desktop builds\n"
           "  --fullscreen             full-screen window for desktop builds\n"
           "  --verbose                include debug messages in the log\n"
           "  --version                print the version and exit\n"
           "  --help                   print this help and exit\n";
}

}  // namespace akeno::app
