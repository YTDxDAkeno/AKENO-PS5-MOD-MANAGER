// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "UiTestSupport.hpp"
#include "akeno/ui/Widgets.hpp"

using namespace akeno;
using namespace akeno::ui;

TEST_CASE("FocusList moves, clamps and scrolls") {
    FocusList list;
    list.setCount(10);
    list.setVisibleRows(4);
    CHECK(list.focus() == 0);
    CHECK_FALSE(list.handle(Action::Up));
    for (int i = 0; i < 5; ++i) list.handle(Action::Down);
    CHECK(list.focus() == 5);
    CHECK(list.firstVisible() == 2);
    list.handle(Action::PageDown);
    CHECK(list.focus() == 9);
    CHECK(list.firstVisible() == 6);
    CHECK_FALSE(list.handle(Action::Down));
    list.setCount(3);
    CHECK(list.focus() == 2);
    CHECK(list.firstVisible() == 0);
    CHECK_FALSE(list.handle(Action::Confirm));
}

TEST_CASE("FocusList with no items ignores input") {
    FocusList list;
    list.setCount(0);
    CHECK_FALSE(list.handle(Action::Down));
    CHECK(list.focus() == 0);
}

TEST_CASE("FocusGrid moves in two dimensions") {
    FocusGrid grid;
    grid.setColumns(5);
    grid.setVisibleRows(2);
    grid.setCount(12);  // rows: 5, 5, 2
    CHECK_FALSE(grid.handle(Action::Left));
    grid.handle(Action::Right);
    grid.handle(Action::Right);
    CHECK(grid.focus() == 2);
    grid.handle(Action::Down);
    CHECK(grid.focus() == 7);
    grid.handle(Action::Down);  // column 2 has no item in the last row -> last item
    CHECK(grid.focus() == 11);
    CHECK(grid.firstVisibleRow() == 1);
    CHECK_FALSE(grid.handle(Action::Down));
    grid.handle(Action::Up);
    CHECK(grid.focus() == 6);
    grid.setFocus(4);
    CHECK_FALSE(grid.handle(Action::Right));  // end of row does not wrap
    CHECK(grid.firstVisibleRow() == 0);
}

TEST_CASE("drawWrappedText wraps on spaces and limits lines") {
    test::RecordingCanvas canvas;
    // 15 px per character: a 150 px box holds 10 characters.
    int lines = drawWrappedText(canvas, "alpha beta gamma delta epsilon", {0, 0, 150, 100}, TextStyle{}, 30, 5);
    CHECK(lines == 4);
    REQUIRE(canvas.texts.size() == 4);
    CHECK(canvas.texts[0] == "alpha beta");
    CHECK(canvas.texts[1] == "gamma");
    CHECK(canvas.texts[2] == "delta");
    CHECK(canvas.texts[3] == "epsilon");

    canvas.clear();
    lines = drawWrappedText(canvas, "alpha beta gamma delta epsilon", {0, 0, 150, 100}, TextStyle{}, 30, 2);
    CHECK(lines == 2);
    CHECK(canvas.texts.back() == "gamma delta epsilon");  // last line gets the rest (drawText shortens it)

    canvas.clear();
    CHECK(drawWrappedText(canvas, "", {0, 0, 150, 100}, TextStyle{}, 30, 2) == 0);
}
