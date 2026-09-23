cmake_minimum_required(VERSION 3.25)
project(shaoDe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)

add_library(shaode_config src/config.cpp src/layout.cpp)
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
endif()
option(SHAODE_BUILD_COMPOSITOR "Build the wlroots compositor" ON)
if(SHAODE_BUILD_COMPOSITOR)
    # The wlroots API is unstable. Target one release series deliberately.
    pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET "wlroots-0.20>=0.20.0" "wlroots-0.20<0.21")
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-server)
    pkg_check_modules(PROTOCOLS REQUIRED wayland-protocols)
    pkg_get_variable(PROTOCOL_DIR wayland-protocols pkgdatadir)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h
        COMMAND ${WAYLAND_SCANNER} server-header
        ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h
        DEPENDS ${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml)
    add_executable(shaode src/main.cpp src/backend.c
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h)
    target_include_directories(shaode PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(shaode PRIVATE WLR_USE_UNSTABLE)
    target_compile_options(shaode PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(shaode PRIVATE shaode_config PkgConfig::WLROOTS PkgConfig::WAYLAND)
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
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c)
        target_include_directories(wayland_probe PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(wayland_probe PRIVATE PkgConfig::WAYLAND_CLIENT)
        target_compile_options(wayland_probe PRIVATE -Wall -Wextra -Wno-unused-parameter)
        add_test(NAME compositor_smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/compositor_smoke.py
            $<TARGET_FILE:shaode> $<TARGET_FILE:wayland_probe>
            ${CMAKE_CURRENT_SOURCE_DIR}/config/init.lua)
        set_tests_properties(compositor_smoke PROPERTIES TIMEOUT 60)
    endif()
endif()
