cmake_minimum_required(VERSION 3.16)
project(backup_system VERSION 1.8.1 LANGUAGES CXX)

option(BACKUP_BUILD_GUI "Build the optional Qt6 GUI" ON)
option(BACKUP_BUILD_TESTS "Build automated tests" ON)

# Match the reference layout; callers can override this for isolated builds.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin" CACHE PATH
    "Directory for executables")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH "")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)

# Packages ship the Debian layout, so the default install prefix is /usr, not
# /usr/local. An explicit -DCMAKE_INSTALL_PREFIX, including the one used by
# `cmake --install --prefix`, still wins.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install path prefix" FORCE)
endif()

find_package(Threads REQUIRED)
set(BACKUP_WARNINGS -Wall -Wextra -Wpedantic)

add_library(backup_core STATIC
    DataBackup/core/common.cpp
    DataBackup/core/sha256.cpp
    DataBackup/core/entry.cpp
    DataBackup/core/pack.cpp
    DataBackup/core/compress_rle.cpp
    DataBackup/core/compress_huffman.cpp
    DataBackup/core/compress_lz77.cpp
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
            GUI/ui.hpp
            GUI/main_window.cpp
            GUI/select_entries.cpp
            GUI/widgets.cpp
            GUI/forms.cpp
            GUI/pages_local.cpp
            GUI/pages_remote.cpp
            GUI/pages_access.cpp
            GUI/pages_account.cpp
            GUI/resources.qrc
        )
        target_include_directories(backup_ui PUBLIC GUI)
        target_link_libraries(backup_ui PUBLIC backup_network Qt6::Widgets)
        target_compile_options(backup_ui PRIVATE ${BACKUP_WARNINGS})
        set_target_properties(backup_ui PROPERTIES AUTOMOC ON AUTORCC ON)
        add_executable(backup-gui GUI/main.cpp)
        target_link_libraries(backup-gui PRIVATE backup_ui)
        target_compile_options(backup-gui PRIVATE ${BACKUP_WARNINGS})
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
        test/test_compress.cpp
        test/test_selection.cpp
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

# ---- Installation and packaging --------------------------------------------
# Components: client (Qt desktop application and command-line client), server
# (shared backup service for a dedicated host), and local (drop-in that keeps
# the service on the loopback interface). The all-in-one "backup-studio"
# package installs every component through one installer; the release script
# also builds the per-component debs with CPACK_COMPONENTS_ALL.
include(GNUInstallDirs)

install(TARGETS backup-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT client)
install(TARGETS backup-server
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT server)
install(FILES README.md CHANGELOG.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/backup-studio
        COMPONENT client)
# Absolute destinations keep the Debian layout (/etc, /lib) for every
# generator; CPack writes them under DESTDIR, so no host path is touched.
install(FILES packaging/databackup.conf
        DESTINATION /etc/backup-studio COMPONENT server)
install(FILES packaging/systemd/backup-studio.service
        DESTINATION /lib/systemd/system COMPONENT server)
install(FILES packaging/systemd/backup-studio.service.d/local.conf
        DESTINATION /lib/systemd/system/backup-studio.service.d
        COMPONENT local)

if(TARGET backup-gui)
    install(TARGETS backup-gui
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT client)
    install(FILES packaging/backup-studio.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
            COMPONENT client)
    install(FILES packaging/backup-studio.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
            COMPONENT client)
endif()

set(CPACK_PACKAGE_NAME "backup-studio")
set(CPACK_PACKAGE_VENDOR "Backup Studio")
set(CPACK_PACKAGE_CONTACT "Backup Studio maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Directory backup and restore with a Qt desktop client and a TCP server")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
set(CPACK_PACKAGE_FILE_NAME
    "backup-studio-${PROJECT_VERSION}-ubuntu24.04-x86_64")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES ON)
# Without this the staging tree follows CMAKE_INSTALL_PREFIX (/usr/local),
# which Debian policy reserves for the local administrator; the tarball and
# the debs must agree on /usr.
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_GENERATOR "TGZ;DEB")
# Install rules use absolute destinations for the Debian layout (/etc, /lib).
# The TGZ generator does not enable DESTDIR by itself, so without this the
# staging install would try to write to the real /etc on the build host.
set(CPACK_SET_DESTDIR ON)

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
set(CPACK_DEBIAN_CLIENT_PACKAGE_NAME "backup-studio-client")
set(CPACK_DEBIAN_SERVER_PACKAGE_NAME "backup-studio-server")
set(CPACK_DEBIAN_LOCAL_PACKAGE_NAME "backup-studio-local")

# Qt loads its platform plugin and the CJK font at run time, so dpkg-shlibdeps
# cannot infer them; without both, the desktop client fails to start or shows
# placeholder boxes. Component variables override the global ones, so the
# server deb must state that it wants neither.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, fonts-noto-cjk")
set(CPACK_DEBIAN_CLIENT_PACKAGE_DEPENDS "qt6-qpa-plugins, fonts-noto-cjk")
set(CPACK_DEBIAN_SERVER_PACKAGE_DEPENDS "")

# Only the server package owns a unit, an account and a conffile, and only it
# may run those maintainer scripts. The all-in-one deb gets the same scripts
# from the release script through -DCPACK_DEBIAN_PACKAGE_CONTROL_EXTRA, which
# keeps them out of the client component.
set(CPACK_DEBIAN_SERVER_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/prerm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/conffiles")

include(CPack)

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
