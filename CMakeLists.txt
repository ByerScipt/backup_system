cmake_minimum_required(VERSION 3.16)
project(backup_system VERSION 1.5.0 LANGUAGES CXX)

option(BACKUP_BUILD_GUI "Build the optional Qt6 GUI" ON)
option(BACKUP_BUILD_TESTS "Build automated tests" ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH "")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)

find_package(Threads REQUIRED)
set(BACKUP_WARNINGS -Wall -Wextra -Wpedantic)

add_library(backup_core STATIC
    src/core/common.cpp
    src/core/sha256.cpp
    src/core/entry.cpp
    src/core/pack.cpp
    src/core/compress_rle.cpp
    src/core/compress_huffman.cpp
    src/core/crypto.cpp
    src/core/archive.cpp
    src/core/restore.cpp
    src/core/engine.cpp
)
target_include_directories(backup_core PUBLIC include)
target_compile_features(backup_core PUBLIC cxx_std_17)

add_library(backup_network STATIC
    src/network/io.cpp
    src/network/auth_store.cpp
    src/network/server.cpp
    src/network/session.cpp
    src/network/client.cpp
)
target_link_libraries(backup_network PUBLIC backup_core Threads::Threads)
target_compile_options(backup_network PRIVATE ${BACKUP_WARNINGS})

target_compile_definitions(backup_core PUBLIC BACKUP_SYSTEM_VERSION="${PROJECT_VERSION}")
target_compile_options(backup_core PRIVATE ${BACKUP_WARNINGS})

add_executable(backup-cli cli/main.cpp cli/arguments.cpp cli/commands.cpp)
target_include_directories(backup-cli PRIVATE cli)
target_link_libraries(backup-cli PRIVATE backup_network)
target_compile_options(backup-cli PRIVATE ${BACKUP_WARNINGS})

add_executable(backup-server server/main.cpp)
target_link_libraries(backup-server PRIVATE backup_network)
target_compile_options(backup-server PRIVATE ${BACKUP_WARNINGS})

if(BACKUP_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        add_executable(backup-gui
            gui/main.cpp
            gui/main_window.cpp
            gui/theme.cpp
            gui/widgets.cpp
            gui/forms.cpp
            gui/dialogs.cpp
            gui/pages_local.cpp
            gui/pages_remote.cpp
            gui/pages_user.cpp
            gui/resources.qrc
        )
        target_include_directories(backup-gui PRIVATE gui)
        target_link_libraries(backup-gui PRIVATE backup_network Qt6::Widgets)
        target_compile_options(backup-gui PRIVATE ${BACKUP_WARNINGS})
        set_target_properties(backup-gui PROPERTIES AUTOMOC ON AUTORCC ON)
        install(TARGETS backup-gui RUNTIME DESTINATION bin)
    else()
        message(WARNING "Qt6 Widgets was not found; backup-gui will not be built")
    endif()
endif()

if(BACKUP_BUILD_TESTS)
    enable_testing()
    add_executable(backup-tests
        tests/test_main.cpp
        tests/helpers.cpp
        tests/test_combos.cpp
        tests/test_security.cpp
        tests/test_network.cpp
        tests/test_crypto_vectors.cpp
    )
    target_include_directories(backup-tests PRIVATE tests src)
    target_link_libraries(backup-tests PRIVATE backup_network)
    target_compile_options(backup-tests PRIVATE ${BACKUP_WARNINGS})
    add_test(NAME backup-tests COMMAND backup-tests)
endif()

install(TARGETS backup-cli backup-server RUNTIME DESTINATION bin)

find_program(CLANG_FORMAT NAMES clang-format-18 clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
        include/*.hpp src/*.hpp src/*.cpp cli/*.hpp cli/*.cpp
        gui/*.hpp gui/*.cpp server/*.cpp tests/*.hpp tests/*.cpp
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
