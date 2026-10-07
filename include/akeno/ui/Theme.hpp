// SPDX-License-Identifier: GPL-3.0-or-later
// Visual constants. Sized for a television viewed from several metres: body text is 32 px at
// 1080p and nothing interactive is smaller than 26 px.
#pragma once

#include "akeno/providers/IModProvider.hpp"
#include "akeno/ui/Canvas.hpp"

namespace akeno::ui::theme {

inline constexpr int kScreenWidth = 1920;
inline constexpr int kScreenHeight = 1080;
inline constexpr int kMargin = 80;

inline constexpr Rect kHeader{0, 0, kScreenWidth, 110};
inline constexpr Rect kTabBar{0, 110, kScreenWidth, 80};
inline constexpr Rect kContent{kMargin, 210, kScreenWidth - 2 * kMargin, 780};
inline constexpr Rect kFooter{0, 1000, kScreenWidth, 80};

inline constexpr int fontSize(FontRole role) {
    switch (role) {
        case FontRole::Display: return 64;
        case FontRole::Title: return 48;
        case FontRole::Heading: return 38;
        case FontRole::Body: return 32;
        case FontRole::Caption: return 27;
        case FontRole::Small: return 24;
    }
    return 32;
}

inline constexpr Color kBackground{11, 15, 26, 255};
inline constexpr Color kBackgroundTop{20, 27, 46, 255};
inline constexpr Color kPanel{24, 31, 48, 255};
inline constexpr Color kPanelRaised{33, 42, 64, 255};
inline constexpr Color kAccent{91, 140, 255, 255};
inline constexpr Color kFocus{255, 255, 255, 255};
inline constexpr Color kTextPrimary{242, 244, 248, 255};
inline constexpr Color kTextSecondary{154, 164, 184, 255};
inline constexpr Color kTextDisabled{96, 104, 122, 255};

inline constexpr Color kOk{46, 158, 91, 255};
inline constexpr Color kWarning{217, 154, 30, 255};
inline constexpr Color kError{214, 69, 69, 255};
inline constexpr Color kNeutral{107, 114, 128, 255};

// DualSense face-button colours used in button hints.
inline constexpr Color kButtonCross{124, 178, 232, 255};
inline constexpr Color kButtonCircle{255, 102, 102, 255};
inline constexpr Color kButtonTriangle{64, 226, 160, 255};
inline constexpr Color kButtonSquare{255, 105, 248, 255};

// Compatibility badge colours (docs/compatibility.md). Uncertain states never use green.
inline constexpr Color badgeColor(providers::CompatibilityStatus status) {
    switch (status) {
        case providers::CompatibilityStatus::Verified: return {46, 158, 91, 255};
        case providers::CompatibilityStatus::Likely: return {42, 157, 168, 255};
        case providers::CompatibilityStatus::Experimental: return {217, 154, 30, 255};
        case providers::CompatibilityStatus::Unknown: return {107, 114, 128, 255};
        case providers::CompatibilityStatus::PcOnly: return {139, 92, 246, 255};
        case providers::CompatibilityStatus::Incompatible: return {214, 69, 69, 255};
    }
    return {107, 114, 128, 255};
}

inline constexpr const char* badgeLabel(providers::CompatibilityStatus status) {
    switch (status) {
        case providers::CompatibilityStatus::Verified: return "VERIFIED";
        case providers::CompatibilityStatus::Likely: return "LIKELY";
        case providers::CompatibilityStatus::Experimental: return "EXPERIMENTAL";
        case providers::CompatibilityStatus::Unknown: return "UNKNOWN";
        case providers::CompatibilityStatus::PcOnly: return "PC ONLY";
        case providers::CompatibilityStatus::Incompatible: return "INCOMPATIBLE";
    }
    return "UNKNOWN";
}

}  // namespace akeno::ui::theme
