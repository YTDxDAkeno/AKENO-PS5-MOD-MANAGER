# SPDX-License-Identifier: GPL-3.0-or-later

# Screens, widgets and navigation. No SDL dependency: rendered through ICanvas, testable on the host.
add_library(akeno_ui STATIC
    Canvas.cpp
    Input.cpp
    Widgets.cpp
    ScreenHost.cpp
    AppController.cpp
    UiScript.cpp
    screens/HomeScreen.cpp
    screens/GameScreens.cpp
    screens/InfoScreens.cpp
    screens/SettingsScreen.cpp
    screens/FlowScreens.cpp
    screens/ModScreens.cpp
    screens/DownloadScreens.cpp
    screens/InstalledModsScreen.cpp
)
target_link_libraries(akeno_ui PUBLIC akeno_core PRIVATE akeno_warnings)

# SDL2 backend.
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf SDL2_image)
add_library(akeno_sdl STATIC
    sdl/SdlCanvas.cpp
    sdl/SdlImageService.cpp
    sdl/SdlInput.cpp
    sdl/SdlApplication.cpp
)
akeno_embed_file(akeno_sdl "${CMAKE_SOURCE_DIR}/assets/fonts/DejaVuSans.ttf" fontRegular bytes)
akeno_embed_file(akeno_sdl "${CMAKE_SOURCE_DIR}/assets/fonts/DejaVuSans-Bold.ttf" fontBold bytes)
target_link_libraries(akeno_sdl PUBLIC akeno_ui PkgConfig::SDL2 PRIVATE akeno_warnings)

add_executable(AkenoModManager "${CMAKE_SOURCE_DIR}/src/app/main.cpp")
target_link_libraries(AkenoModManager PRIVATE akeno_sdl akeno_warnings)

if(AKENO_TARGET_PS5)
    set_target_properties(AkenoModManager PROPERTIES OUTPUT_NAME "AkenoModManager.elf")

    # websrv homebrew folder (launched from the Homebrew Launcher) and the raw payload.
    set(dist "${CMAKE_BINARY_DIR}/dist")
    set(hb "${dist}/AkenoModManager")
    add_custom_target(homebrew_package ALL
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${hb}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${hb}/sce_sys"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:AkenoModManager>" "${hb}/eboot.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/assets/homebrew/homebrew.js" "${hb}/homebrew.js"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/assets/icon0.png" "${hb}/sce_sys/icon0.png"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:AkenoModManager>" "${dist}/AkenoModManager.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:AkenoSelfCheck>" "${dist}/AkenoSelfCheck.elf"
        # Debug information only; symbol tables stay for the payload loaders.
        COMMAND "${CMAKE_STRIP}" --strip-debug "${hb}/eboot.elf"
        COMMAND "${CMAKE_STRIP}" --strip-debug "${dist}/AkenoModManager.elf"
        COMMAND "${CMAKE_STRIP}" --strip-debug "${dist}/AkenoSelfCheck.elf"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${dist}"
                "${CMAKE_COMMAND}" -E tar cf "AkenoModManager-homebrew.zip" --format=zip AkenoModManager
        COMMAND "${CMAKE_COMMAND}" -DDIRECTORY=${dist} -DFILES=AkenoModManager.elf,AkenoSelfCheck.elf,AkenoModManager-homebrew.zip
                -P "${CMAKE_SOURCE_DIR}/cmake/Checksums.cmake"
        DEPENDS AkenoModManager AkenoSelfCheck
        COMMENT "Packaging dist/AkenoModManager.elf, dist/AkenoModManager-homebrew.zip and dist/SHA256SUMS"
        VERBATIM)
endif()
