cmake_minimum_required(VERSION 3.16)
project(backup_system VERSION 1.7.0 LANGUAGES CXX)

option(BACKUP_BUILD_GUI "Build the optional Qt6 GUI" ON)
option(BACKUP_BUILD_TESTS "Build automated tests" ON)

# Match the reference layout; callers can override this for isolated builds.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin" CACHE PATH
    "Directory for executables")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH "")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)

find_package(Threads REQUIRED)
set(BACKUP_WARNINGS -Wall -Wextra -Wpedantic)

add_library(backup_core STATIC
    DataBackup/core/common.cpp
    DataBackup/core/sha256.cpp
    DataBackup/core/entry.cpp
    DataBackup/core/pack.cpp
    DataBackup/core/compress_rle.cpp
    DataBackup/core/compress_huffman.cpp
    DataBackup/core/crypto.cpp
    DataBackup/core/archive.cpp
    DataBackup/core/restore.cpp
    DataBackup/core/engine.cpp
)
target_include_directories(backup_core PUBLIC libs)
target_compile_features(backup_core PUBLIC cxx_std_17)

add_library(backup_network STATIC
    DataBackup/network/io.cpp
    DataBackup/network/auth_store.cpp
    DataBackup/network/server.cpp
    DataBackup/network/session.cpp
    DataBackup/network/client.cpp
)
target_link_libraries(backup_network PUBLIC backup_core Threads::Threads)
target_compile_options(backup_network PRIVATE ${BACKUP_WARNINGS})

target_compile_definitions(backup_core PUBLIC BACKUP_SYSTEM_VERSION="${PROJECT_VERSION}")
target_compile_options(backup_core PRIVATE ${BACKUP_WARNINGS})

add_executable(backup-cli DataBackup/cli/main.cpp DataBackup/cli/arguments.cpp DataBackup/cli/commands.cpp)
target_include_directories(backup-cli PRIVATE DataBackup/cli)
target_link_libraries(backup-cli PRIVATE backup_network)
target_compile_options(backup-cli PRIVATE ${BACKUP_WARNINGS})

add_executable(backup-server DataBackup/server.cpp)
target_link_libraries(backup-server PRIVATE backup_network)
target_compile_options(backup-server PRIVATE ${BACKUP_WARNINGS})

if(BACKUP_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        add_library(backup_ui OBJECT
            GUI/main_window.cpp
            GUI/widgets.cpp
            GUI/forms.cpp
            GUI/pages_local.cpp
            GUI/pages_remote.cpp
            GUI/pages_access.cpp
            GUI/resources.qrc
        )
        target_include_directories(backup_ui PUBLIC GUI)
        target_link_libraries(backup_ui PUBLIC backup_network Qt6::Widgets)
        target_compile_options(backup_ui PRIVATE ${BACKUP_WARNINGS})
        set_target_properties(backup_ui PROPERTIES AUTOMOC ON AUTORCC ON)
        add_executable(backup-gui GUI/main.cpp)
        target_link_libraries(backup-gui PRIVATE backup_ui)
        target_compile_options(backup-gui PRIVATE ${BACKUP_WARNINGS})
        install(TARGETS backup-gui RUNTIME DESTINATION bin)
    else()
        message(WARNING "Qt6 Widgets was not found; backup-gui will not be built")
    endif()
endif()

if(BACKUP_BUILD_TESTS)
    enable_testing()
    add_executable(backup-tests
        test/test_main.cpp
        test/helpers.cpp
        test/test_combos.cpp
        test/test_security.cpp
        test/test_network.cpp
        test/test_crypto_vectors.cpp
        test/test_filesystem.cpp
        test/test_io.cpp
        test/test_network_failures.cpp
    )
    target_include_directories(backup-tests PRIVATE test DataBackup)
    target_link_libraries(backup-tests PRIVATE backup_network)
    # Inject failures at the operating-system boundary, not inside backup_core.
    target_link_options(backup-tests PRIVATE "-Wl,--wrap=fsync")
    target_compile_options(backup-tests PRIVATE ${BACKUP_WARNINGS})
    add_test(NAME backup-tests COMMAND backup-tests)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME backup-cli-tests COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test/test_cli.py
            $<TARGET_FILE:backup-cli> $<TARGET_FILE:backup-server>)
    endif()
    if(TARGET backup-gui)
        find_package(Qt6 COMPONENTS Test Network REQUIRED)
        add_executable(backup-gui-tests test/test_gui_jobs.cpp)
        target_link_libraries(backup-gui-tests PRIVATE backup_ui Qt6::Test Qt6::Network)
        add_dependencies(backup-gui-tests backup-server)
        target_compile_options(backup-gui-tests PRIVATE ${BACKUP_WARNINGS})
        add_test(NAME backup-gui-tests COMMAND backup-gui-tests)
        set_tests_properties(backup-gui-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    endif()
endif()

install(TARGETS backup-cli backup-server RUNTIME DESTINATION bin)

find_program(CLANG_FORMAT NAMES clang-format-18 clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
        libs/*.hpp DataBackup/*.hpp DataBackup/*.cpp
        GUI/*.hpp GUI/*.cpp test/*.hpp test/*.cpp
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${FORMAT_SOURCES}
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${FORMAT_SOURCES}
        VERBATIM
    )
endif()
