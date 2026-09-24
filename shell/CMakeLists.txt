find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(SHELL_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
if(NOT SHAODE_SHELL_PREVIEW_ONLY)
    find_package(LayerShellQt 6.6 QUIET)
    if(NOT LayerShellQt_FOUND)
        message(FATAL_ERROR "The desktop shell requires LayerShellQt 6.6+. Install layer-shell-qt, or use -DSHAODE_BUILD_SHELL=OFF for the compositor or -DSHAODE_SHELL_PREVIEW_ONLY=ON for UI development.")
    endif()
endif()

add_executable(shaode-shell main.cpp controller.cpp controller.hpp task_model.cpp task_model.hpp
    view.cpp view.hpp
    ${PROJECT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
    ${PROJECT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h)
set_target_properties(shaode-shell PROPERTIES AUTOMOC ON RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
target_include_directories(shaode-shell PRIVATE ${PROJECT_BINARY_DIR})
target_link_libraries(shaode-shell PRIVATE shaode_config Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick
    PkgConfig::GIO PkgConfig::SHELL_WAYLAND)
target_compile_definitions(shaode-shell PRIVATE QT_NO_KEYWORDS)
target_compile_options(shaode-shell PRIVATE -Wall -Wextra -Wpedantic)
if(SHAODE_SHELL_PREVIEW_ONLY)
    target_compile_definitions(shaode-shell PRIVATE SHAODE_LAYER_SHELL=0)
else()
    target_link_libraries(shaode-shell PRIVATE LayerShellQt::Interface)
    target_compile_definitions(shaode-shell PRIVATE SHAODE_LAYER_SHELL=1)
    install(TARGETS shaode-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
qt_add_resources(shaode-shell shell_qml PREFIX "/shell" BASE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES Panel.qml Desktop.qml)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_executable(shell_ui_test ${PROJECT_SOURCE_DIR}/tests/shell_ui_test.cpp
        controller.cpp controller.hpp view.cpp view.hpp task_model.cpp task_model.hpp
        ${PROJECT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
        ${PROJECT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h)
    set_target_properties(shell_ui_test PROPERTIES AUTOMOC ON)
    target_include_directories(shell_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_BINARY_DIR})
    target_compile_definitions(shell_ui_test PRIVATE QT_NO_KEYWORDS SHAODE_LAYER_SHELL=0)
    target_compile_options(shell_ui_test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(shell_ui_test PRIVATE shaode_config Qt6::Test Qt6::Network Qt6::Quick Qt6::Qml
        PkgConfig::GIO PkgConfig::SHELL_WAYLAND)
    qt_add_resources(shell_ui_test ui_test_qml PREFIX "/shell" BASE ${CMAKE_CURRENT_SOURCE_DIR}
        FILES Panel.qml Desktop.qml)
    add_test(NAME shell_ui COMMAND shell_ui_test ${CMAKE_COMMAND})
    set_tests_properties(shell_ui PROPERTIES TIMEOUT 20
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    add_executable(task_model_test ${PROJECT_SOURCE_DIR}/tests/task_model_test.cpp
        task_model.cpp task_model.hpp
        ${PROJECT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c
        ${PROJECT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h)
    set_target_properties(task_model_test PROPERTIES AUTOMOC ON)
    target_include_directories(task_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_BINARY_DIR})
    target_compile_definitions(task_model_test PRIVATE QT_NO_KEYWORDS)
    target_compile_options(task_model_test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(task_model_test PRIVATE Qt6::Core PkgConfig::SHELL_WAYLAND)
    add_test(NAME shell_tasks COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/compositor_smoke.py $<TARGET_FILE:shaode>
        $<TARGET_FILE:wayland_probe> ${PROJECT_SOURCE_DIR}/config/init.lua
        $<TARGET_FILE:task_model_test>)
    set_tests_properties(shell_tasks PROPERTIES TIMEOUT 60)
    add_test(NAME shell_preview COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/shell_preview.py $<TARGET_FILE:shaode-shell>
        ${PROJECT_SOURCE_DIR}/config/init.lua)
    set_tests_properties(shell_preview PROPERTIES TIMEOUT 20)
    if(NOT SHAODE_SHELL_PREVIEW_ONLY)
        add_test(NAME shell_integration COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/shell_integration.py $<TARGET_FILE:shaode>
            $<TARGET_FILE:shaode-shell> $<TARGET_FILE:wayland_probe>
            ${PROJECT_SOURCE_DIR}/config/init.lua)
        set_tests_properties(shell_integration PROPERTIES TIMEOUT 30)
    endif()
endif()
