# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)

project(AkenoPS5ModManager VERSION 0.1.0 LANGUAGES C CXX)

set(AKENO_VERSION_STRING "0.1.0-alpha")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PS5 OR PROSPERO)
    set(AKENO_TARGET_PS5 ON)
    set(AKENO_TARGET_NAME "ps5")
else()
    set(AKENO_TARGET_PS5 OFF)
    set(AKENO_TARGET_NAME "host")
endif()

option(AKENO_BUILD_APP "Build the SDL2 user interface and the executable" ON)
if(AKENO_TARGET_PS5)
    option(AKENO_BUILD_TESTS "Build the unit tests" OFF)
else()
    option(AKENO_BUILD_TESTS "Build the unit tests" ON)
endif()
option(AKENO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(AKENO_SANITIZE "Build host tests with AddressSanitizer and UBSan" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------------- version info
set(AKENO_GIT_REVISION "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE AKENO_GIT_REVISION
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT AKENO_GIT_REVISION)
        set(AKENO_GIT_REVISION "unknown")
    endif()
endif()
configure_file(src/core/BuildInfo.cpp.in "${CMAKE_BINARY_DIR}/generated/BuildInfo.cpp" @ONLY)

# ------------------------------------------------------------------------- warnings
add_library(akeno_warnings INTERFACE)
target_compile_options(akeno_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                       -Wno-sign-conversion -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2)
if(AKENO_WARNINGS_AS_ERRORS)
    target_compile_options(akeno_warnings INTERFACE -Werror)
endif()

# ------------------------------------------------------------------------- dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)

if(AKENO_TARGET_PS5)
    # pacbrew installs SQLite without a pkg-config file.
    set(AKENO_PS5_HBROOT "${PS5_PAYLOAD_SDK}/target/user/homebrew")
    add_library(akeno_sqlite3 INTERFACE)
    target_include_directories(akeno_sqlite3 INTERFACE "${AKENO_PS5_HBROOT}/include")
    target_link_libraries(akeno_sqlite3 INTERFACE "${AKENO_PS5_HBROOT}/lib/libsqlite3.a")
else()
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
    add_library(akeno_sqlite3 INTERFACE)
    target_link_libraries(akeno_sqlite3 INTERFACE PkgConfig::SQLITE3)
endif()

add_library(akeno_third_party INTERFACE)
target_include_directories(akeno_third_party SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party")

include(cmake/Embed.cmake)

# ------------------------------------------------------------------------- akeno_core
add_library(akeno_core STATIC
    src/core/Result.cpp
    src/core/Strings.cpp
    src/core/Json.cpp
    src/core/Tasks.cpp
    src/core/OperationJournal.cpp
    "${CMAKE_BINARY_DIR}/generated/BuildInfo.cpp"
    src/logging/Logger.cpp
    src/logging/Redactor.cpp
    src/security/PathGuard.cpp
    src/security/SafeName.cpp
    src/security/SafeFs.cpp
    src/security/ImageProbe.cpp
    src/security/Sha256.cpp
    src/platform/PlatformCommon.cpp
    src/network/Url.cpp
    src/network/Http.cpp
    src/network/CurlHttpClient.cpp
    src/database/Database.cpp
    src/database/Migrations.cpp
    src/database/SettingsStore.cpp
    src/games/GameInfo.cpp
    src/games/GameLibrary.cpp
    src/shadowmount/ShadowMountClient.cpp
    src/shadowmount/ShadowMountGameProvider.cpp
    src/mods/Catalog.cpp
    src/compatibility/CompatibilityRules.cpp
    src/providers/AkenoCatalogProvider.cpp
    src/downloads/DownloadTypes.cpp
    src/downloads/DownloadStore.cpp
    src/downloads/DownloadManager.cpp
    src/app/CommandLine.cpp
    src/app/SystemCheck.cpp
    src/app/AppContext.cpp
    src/app/Headless.cpp
)
if(AKENO_TARGET_PS5)
    target_sources(akeno_core PRIVATE src/platform/PlatformPs5.cpp)
    akeno_embed_file(akeno_core "${PS5_PAYLOAD_SDK}/target/user/homebrew/etc/ca-bundle.crt" caBundlePem text)
    target_compile_definitions(akeno_core PUBLIC AKENO_TARGET_PS5=1)
    target_link_libraries(akeno_core PUBLIC SceSystemService)
else()
    target_sources(akeno_core PRIVATE src/platform/PlatformHost.cpp src/core/EmbeddedCaHost.cpp)
endif()
target_include_directories(akeno_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
# The harmless file of `--download-test` (tools/make_download_test.py): its size and SHA-256 are
# compiled in, so the test checks exactly the file in this repository.
set(AKENO_DOWNLOAD_TEST_FILE "${CMAKE_SOURCE_DIR}/assets/test/download-test.zip")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AKENO_DOWNLOAD_TEST_FILE}")
file(SHA256 "${AKENO_DOWNLOAD_TEST_FILE}" AKENO_DOWNLOAD_TEST_SHA256)
file(SIZE "${AKENO_DOWNLOAD_TEST_FILE}" AKENO_DOWNLOAD_TEST_SIZE)
set_source_files_properties(src/app/Headless.cpp PROPERTIES COMPILE_DEFINITIONS
    "AKENO_DOWNLOAD_TEST_SHA256=\"${AKENO_DOWNLOAD_TEST_SHA256}\";AKENO_DOWNLOAD_TEST_SIZE=${AKENO_DOWNLOAD_TEST_SIZE}")
target_link_libraries(akeno_core
    PUBLIC akeno_third_party
    PRIVATE akeno_warnings PkgConfig::CURL PkgConfig::CRYPTO akeno_sqlite3)
find_package(Threads REQUIRED)
target_link_libraries(akeno_core PUBLIC Threads::Threads)

if(AKENO_SANITIZE AND NOT AKENO_TARGET_PS5)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    target_compile_options(akeno_core PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(akeno_core PUBLIC -fsanitize=address,undefined)
endif()

# ------------------------------------------------------------------------- headless self-check payload
# Minimal ELF without SDL for the first hardware test; works with loaders that cannot pass arguments.
add_executable(AkenoSelfCheck src/app/selfcheck_main.cpp)
target_link_libraries(AkenoSelfCheck PRIVATE akeno_core akeno_warnings)
if(AKENO_TARGET_PS5)
    set_target_properties(AkenoSelfCheck PROPERTIES OUTPUT_NAME "AkenoSelfCheck.elf")
endif()

# ------------------------------------------------------------------------- catalogue maintenance tool
# Host-only: validates catalogue directories with the application's own parser.
if(NOT AKENO_TARGET_PS5)
    add_executable(akeno-catalog-check tools/catalog_check/main.cpp)
    target_link_libraries(akeno-catalog-check PRIVATE akeno_core akeno_warnings)
endif()

# ------------------------------------------------------------------------- user interface
if(AKENO_BUILD_APP)
    add_subdirectory(src/ui)
endif()

# ------------------------------------------------------------------------- tests
if(AKENO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    add_test(NAME demo_catalogue_is_consistent
             COMMAND akeno-catalog-check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/catalog" --allow-loopback)
    add_test(NAME published_catalogue_is_consistent
             COMMAND akeno-catalog-check "${CMAKE_CURRENT_SOURCE_DIR}/catalog")
    # The demo catalogue uses http://127.0.0.1: the strict (published) rules must reject it.
    add_test(NAME catalogue_check_rejects_plain_http
             COMMAND akeno-catalog-check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/catalog")
    set_tests_properties(catalogue_check_rejects_plain_http PROPERTIES WILL_FAIL TRUE)
endif()
