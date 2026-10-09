# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(akeno_tests
    test_main.cpp
    support/LocalHttpServer.cpp
    core/test_strings.cpp
    core/test_json.cpp
    core/test_tasks.cpp
    core/test_operation_journal.cpp
    logging/test_redactor.cpp
    logging/test_logger.cpp
    security/test_path_guard.cpp
    security/test_safe_fs.cpp
    security/test_image_probe.cpp
    security/test_sha256.cpp
    security/test_digests.cpp
    security/test_safe_open.cpp
    platform/test_platform.cpp
    network/test_url.cpp
    network/test_http_policy.cpp
    network/test_curl_client.cpp
    database/test_migrations.cpp
    database/test_settings.cpp
    games/test_games.cpp
    games/test_param_json.cpp
    shadowmount/test_shadowmount.cpp
    mods/test_catalog.cpp
    compatibility/test_compatibility_rules.cpp
    compatibility/test_compatibility_engine.cpp
    games/test_game_tree.cpp
    providers/test_catalog_provider.cpp
    providers/test_nexus_provider.cpp
    providers/test_gamebanana_provider.cpp
    downloads/test_downloads.cpp
    archives/test_secure_extractor.cpp
    mods/test_mod_analyzer.cpp
    mods/test_mod_check.cpp
    mods/test_archive_layout.cpp
    mods/test_bcw_case.cpp
    unreal/test_unreal_formats.cpp
    unreal/test_unreal_analyzer.cpp
    install/test_overlay_manager.cpp
    diagnostics/test_diagnostic_export.cpp
    app/test_system_check.cpp
    app/test_command_line.cpp
)
if(TARGET akeno_ui)
    target_sources(akeno_tests PRIVATE
        ui/test_input.cpp
        ui/test_widgets.cpp
        ui/test_screens.cpp
        ui/test_mod_screens.cpp
        ui/test_app_controller.cpp
    )
    target_link_libraries(akeno_tests PRIVATE akeno_ui)
endif()

target_include_directories(akeno_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(akeno_tests PRIVATE AKENO_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(akeno_tests PRIVATE akeno_core akeno_warnings akeno_sqlite3 PkgConfig::CURL PkgConfig::CRYPTO
                                          PkgConfig::ARCHIVE)

add_test(NAME akeno_tests COMMAND akeno_tests)

if(NOT AKENO_TARGET_PS5)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME offline_overlay_diagnostics
             COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
                     -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -v)
endif()
