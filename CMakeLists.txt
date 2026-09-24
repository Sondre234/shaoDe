cmake_minimum_required(VERSION 3.25)
project(shaoDe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_search_module(LUA REQUIRED IMPORTED_TARGET lua5.4 lua-5.4 lua54 lua)
if(LUA_VERSION VERSION_LESS 5.4 OR NOT LUA_VERSION VERSION_LESS 5.5)
    message(FATAL_ERROR "shaoDe requires Lua 5.4; found ${LUA_VERSION}")
endif()
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)
include(GNUInstallDirs)
option(SHAODE_INSTALL_SESSION "Install the experimental display-manager session entry" OFF)
option(SHAODE_BUILD_SHELL "Build the Qt Quick desktop shell" ON)
option(SHAODE_SHELL_PREVIEW_ONLY "Build only the shell UI preview without LayerShellQt" OFF)

add_library(shaode_config src/config.cpp src/layout.cpp src/tiling.cpp)
target_include_directories(shaode_config PUBLIC include)
target_link_libraries(shaode_config PRIVATE PkgConfig::LUA PUBLIC PkgConfig::XKB)
target_compile_options(shaode_config PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(config_tests tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE shaode_config)
    add_test(NAME config COMMAND config_tests ${CMAKE_CURRENT_SOURCE_DIR}/config/init.lua)
    add_executable(layout_tests tests/layout_tests.cpp)
    target_link_libraries(layout_tests PRIVATE shaode_config)
    add_test(NAME layout COMMAND layout_tests)
    add_executable(tiling_tests tests/tiling_tests.cpp)
    target_link_libraries(tiling_tests PRIVATE shaode_config)
    add_test(NAME tiling COMMAND tiling_tests)
endif()
option(SHAODE_BUILD_COMPOSITOR "Build the wlroots compositor" ON)
if(SHAODE_BUILD_COMPOSITOR)
    # The wlroots API is unstable. Target one release series deliberately.
    pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET "wlroots-0.20>=0.20.0" "wlroots-0.20<0.21")
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-server)
    pkg_check_modules(PROTOCOLS REQUIRED wayland-protocols)
    pkg_get_variable(PROTOCOL_DIR wayland-protocols pkgdatadir)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(WLR_PROTOCOL_HEADERS)
    set(WLR_CLIENT_PROTOCOL_SOURCES)
    foreach(PROTOCOL wlr-layer-shell-unstable-v1 wlr-foreign-toplevel-management-unstable-v1)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-protocol.h
            COMMAND ${WAYLAND_SCANNER} server-header
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml
            ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-protocol.h
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml)
        list(APPEND WLR_PROTOCOL_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-protocol.h)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} client-header
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml
            ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-client-protocol.h
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-protocol.c
            COMMAND ${WAYLAND_SCANNER} private-code
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml
            ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-protocol.c
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/${PROTOCOL}.xml)
        list(APPEND WLR_CLIENT_PROTOCOL_SOURCES
            ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-client-protocol.h
            ${CMAKE_CURRENT_BINARY_DIR}/${PROTOCOL}-protocol.c)
    endforeach()
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h
        COMMAND ${WAYLAND_SCANNER} server-header
        ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h
        DEPENDS ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml)
    add_executable(shaode src/main.cpp src/backend.c src/decoration.c
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h ${WLR_PROTOCOL_HEADERS})
    target_include_directories(shaode PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(shaode PRIVATE WLR_USE_UNSTABLE)
    target_compile_definitions(shaode PRIVATE
        SHAODE_VERSION="${PROJECT_VERSION}"
        SHAODE_DEFAULT_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/shaode/init.lua")
    target_compile_options(shaode PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(shaode PRIVATE shaode_config PkgConfig::WLROOTS PkgConfig::WAYLAND m)
    # X11 support follows the wlroots build; the XWM waker needs xcb directly.
    find_file(SHAODE_WLR_CONFIG_H wlr/config.h PATHS ${WLROOTS_INCLUDE_DIRS} NO_DEFAULT_PATH)
    file(READ ${SHAODE_WLR_CONFIG_H} SHAODE_WLR_CONFIG)
    if(SHAODE_WLR_CONFIG MATCHES "WLR_HAS_XWAYLAND 1")
        # Unpatched wlroots strands X events; see packaging/patches/wlroots-xwm-drain.patch.
        option(SHAODE_XWM_WAKER "Nudge wlroots' X11 window manager (off only for patched wlroots)" ON)
        pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
        target_link_libraries(shaode PRIVATE PkgConfig::XCB)
        if(SHAODE_XWM_WAKER)
            pkg_check_modules(XCB_XFIXES REQUIRED IMPORTED_TARGET xcb-xfixes)
            target_link_libraries(shaode PRIVATE PkgConfig::XCB_XFIXES)
            target_compile_definitions(shaode PRIVATE SHAODE_XWM_WAKER=1)
        endif()
        set(SHAODE_HAS_XWAYLAND ON)
    else()
        message(STATUS "wlroots lacks XWayland support; X11 applications will not run")
    endif()
    install(TARGETS shaode RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES config/init.lua DESTINATION ${CMAKE_INSTALL_DATADIR}/shaode)
    install(FILES README.md docs/gentoo.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/shaode)
    install(FILES vendor/tinywl/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/shaode
        RENAME TinyWL-LICENSE)
    install(DIRECTORY protocols/ DESTINATION ${CMAKE_INSTALL_DATADIR}/shaode/protocols)
    install(FILES packaging/shaode-portals.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
    if(SHAODE_INSTALL_SESSION)
        configure_file(packaging/shaode.desktop.in shaode.desktop @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/shaode.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)
    endif()
    if(SHAODE_BUILD_SHELL)
        add_subdirectory(shell)
        if(NOT SHAODE_SHELL_PREVIEW_ONLY)
            target_compile_definitions(shaode PRIVATE SHAODE_HAS_SHELL=1)
        endif()
    endif()
    if(BUILD_TESTING)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} client-header
            ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
            DEPENDS ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
            COMMAND ${WAYLAND_SCANNER} private-code
            ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
            DEPENDS ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml)
        add_executable(wayland_probe tests/wayland_probe.c
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c ${WLR_CLIENT_PROTOCOL_SOURCES})
        target_include_directories(wayland_probe PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(wayland_probe PRIVATE PkgConfig::WAYLAND_CLIENT)
        target_compile_options(wayland_probe PRIVATE -Wall -Wextra -Wno-unused-parameter)
        add_test(NAME compositor_smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/compositor_smoke.py
            $<TARGET_FILE:shaode> $<TARGET_FILE:wayland_probe>
            ${CMAKE_CURRENT_SOURCE_DIR}/config/init.lua)
        set_tests_properties(compositor_smoke PROPERTIES TIMEOUT 60)
        add_test(NAME workspaces COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/workspace_smoke.py
            $<TARGET_FILE:shaode> $<TARGET_FILE:wayland_probe>
            ${CMAKE_CURRENT_SOURCE_DIR}/config/init.lua)
        set_tests_properties(workspaces PROPERTIES TIMEOUT 60)
        add_test(NAME tiling_smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/tiling_smoke.py
            $<TARGET_FILE:shaode> $<TARGET_FILE:wayland_probe>
            ${CMAKE_CURRENT_SOURCE_DIR}/config/init.lua)
        set_tests_properties(tiling_smoke PROPERTIES TIMEOUT 60)
        set(LOCK_XML ${PROTOCOL_DIR}/staging/ext-session-lock/ext-session-lock-v1.xml)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ext-session-lock-v1-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} client-header ${LOCK_XML}
            ${CMAKE_CURRENT_BINARY_DIR}/ext-session-lock-v1-client-protocol.h
            DEPENDS ${LOCK_XML})
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ext-session-lock-v1-protocol.c
            COMMAND ${WAYLAND_SCANNER} private-code ${LOCK_XML}
            ${CMAKE_CURRENT_BINARY_DIR}/ext-session-lock-v1-protocol.c
            DEPENDS ${LOCK_XML})
        add_executable(lock_probe tests/lock_probe.c
            ${CMAKE_CURRENT_BINARY_DIR}/ext-session-lock-v1-client-protocol.h
            ${CMAKE_CURRENT_BINARY_DIR}/ext-session-lock-v1-protocol.c
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c)
        target_include_directories(lock_probe PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(lock_probe PRIVATE PkgConfig::WAYLAND_CLIENT)
        target_compile_options(lock_probe PRIVATE -Wall -Wextra -Wno-unused-parameter)
        add_test(NAME session_lock COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/lock_smoke.py
            $<TARGET_FILE:shaode> $<TARGET_FILE:lock_probe>
            ${CMAKE_CURRENT_SOURCE_DIR}/config/init.lua)
        set_tests_properties(session_lock PROPERTIES TIMEOUT 60)
        # XWayland coverage needs wlroots built with X support and an Xwayland binary.
        find_program(XWAYLAND_BINARY Xwayland)
        if(SHAODE_HAS_XWAYLAND AND XWAYLAND_BINARY)
            add_executable(x11_probe tests/x11_probe.c)
            target_link_libraries(x11_probe PRIVATE PkgConfig::XCB)
            target_compile_options(x11_probe PRIVATE -Wall -Wextra)
            add_test(NAME xwayland_smoke COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/xwayland_smoke.py
                $<TARGET_FILE:shaode> $<TARGET_FILE:x11_probe> $<TARGET_FILE:wayland_probe>
                ${CMAKE_CURRENT_SOURCE_DIR}/config/init.lua)
            set_tests_properties(xwayland_smoke PROPERTIES TIMEOUT 60)
        else()
            message(STATUS "XWayland test skipped: needs an Xwayland binary and wlroots X support")
        endif()
    endif()
endif()
