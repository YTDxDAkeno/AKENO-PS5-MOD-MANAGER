// SPDX-License-Identifier: GPL-3.0-or-later
// Akeno defines toString() overloads for its enums. doctest stringifies values with an
// unqualified toString() call, so ADL would pick Akeno's functions; qualify it instead.
#pragma once

#define DOCTEST_STRINGIFY(...) ::doctest::toString(__VA_ARGS__)
#include <doctest/doctest.h>
