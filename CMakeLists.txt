cmake_minimum_required(VERSION 3.16)
project(backup_system VERSION 1.3.0 LANGUAGES CXX)

option(BACKUP_BUILD_GUI "Build the optional Qt6 GUI" ON)
option(BACKUP_BUILD_TESTS "Build automated tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH "")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)

find_package(Threads REQUIRED)
set(BACKUP_WARNINGS -Wall -Wextra -Wpedantic)

add_library(backup_core STATIC
    src/core.cpp
    src/network.cpp
)
target_include_directories(backup_core PUBLIC include)
target_link_libraries(backup_core PUBLIC Threads::Threads)
target_compile_definitions(backup_core PUBLIC BACKUP_SYSTEM_VERSION="${PROJECT_VERSION}")
target_compile_options(backup_core PRIVATE ${BACKUP_WARNINGS})

add_executable(backup-cli cli/main.cpp)
target_link_libraries(backup-cli PRIVATE backup_core)
target_compile_options(backup-cli PRIVATE ${BACKUP_WARNINGS})

add_executable(backup-server server/main.cpp)
target_link_libraries(backup-server PRIVATE backup_core)
target_compile_options(backup-server PRIVATE ${BACKUP_WARNINGS})

if(BACKUP_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        add_executable(backup-gui gui/main.cpp)
        target_link_libraries(backup-gui PRIVATE backup_core Qt6::Widgets)
        target_compile_options(backup-gui PRIVATE ${BACKUP_WARNINGS})
        set_target_properties(backup-gui PROPERTIES AUTOMOC ON)
        install(TARGETS backup-gui RUNTIME DESTINATION bin)
    else()
        message(WARNING "Qt6 Widgets was not found; backup-gui will not be built")
    endif()
endif()

if(BACKUP_BUILD_TESTS)
    enable_testing()
    add_executable(backup-tests tests/test_main.cpp)
    target_link_libraries(backup-tests PRIVATE backup_core)
    target_compile_options(backup-tests PRIVATE ${BACKUP_WARNINGS})
    add_test(NAME backup-tests COMMAND backup-tests)
endif()

install(TARGETS backup-cli backup-server RUNTIME DESTINATION bin)
