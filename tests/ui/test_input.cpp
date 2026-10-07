// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/ui/Input.hpp"

using namespace akeno::ui;

TEST_CASE("held directions repeat after a delay, other buttons do not") {
    KeyRepeater repeater(0.3, 0.1);
    CHECK(repeater.press(Action::Down, 0.0) == Action::Down);
    CHECK(repeater.update(0.1).empty());
    CHECK(repeater.update(0.29).empty());
    auto first = repeater.update(0.31);
    REQUIRE(first.size() == 1);
    CHECK(first[0] == Action::Down);
    CHECK(repeater.update(0.35).empty());
    CHECK(repeater.update(0.42).size() == 1);
    repeater.release(Action::Down);
    CHECK(repeater.update(1.0).empty());

    repeater.press(Action::Confirm, 0.0);
    CHECK(repeater.update(5.0).empty());
}

TEST_CASE("a long stall delivers only one repeat") {
    KeyRepeater repeater(0.3, 0.1);
    repeater.press(Action::Right, 0.0);
    CHECK(repeater.update(10.0).size() == 1);
}

TEST_CASE("releaseAll stops every repeat") {
    KeyRepeater repeater(0.1, 0.1);
    repeater.press(Action::Up, 0.0);
    repeater.press(Action::Left, 0.0);
    repeater.releaseAll();
    CHECK(repeater.update(1.0).empty());
}

TEST_CASE("raw PS5 pad buttons follow the SDL port's order") {
    CHECK(ps5RawButtonAction(0) == Action::Confirm);
    CHECK(ps5RawButtonAction(1) == Action::Back);
    CHECK(ps5RawButtonAction(2) == Action::Tertiary);
    CHECK(ps5RawButtonAction(3) == Action::Secondary);
    CHECK(ps5RawButtonAction(6) == Action::Options);
    CHECK(ps5RawButtonAction(9) == Action::PrevTab);
    CHECK(ps5RawButtonAction(10) == Action::NextTab);
    CHECK(ps5RawButtonAction(11) == Action::Up);
    CHECK(ps5RawButtonAction(14) == Action::Right);
    CHECK(ps5RawButtonAction(16) == Action::PageDown);
    CHECK_FALSE(ps5RawButtonAction(4).has_value());  // touchpad: unused
    CHECK_FALSE(ps5RawButtonAction(99).has_value());
}
