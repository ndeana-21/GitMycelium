cmake_minimum_required(VERSION 3.21)

project(gitmycelium VERSION 1.1.1
        DESCRIPTION "A fast, native Git client for large repositories"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test Svg Network)

# The release workflow's run number. It tells two builds of one version apart: the
# updater compares it, and macOS keys its icon and launch caches on CFBundleVersion.
set(GMC_BUILD_NUMBER 0 CACHE STRING "CI build number (0 for a local build)")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)

if(NOT MSVC)
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(bench)

include(GNUInstallDirs)

if(APPLE)
    # The bundle is the application; CPack's DragNDrop generator wraps it and
    # `macdeployqt` has already put the Qt frameworks inside it.
    install(TARGETS gitmycelium BUNDLE DESTINATION .)
    install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md DESTINATION Licenses)
    install(FILES third_party/lucide/LICENSE DESTINATION Licenses RENAME LICENSE-lucide)
else()
    install(TARGETS gitmycelium RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/gitmycelium.desktop
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES packaging/icons/gitmycelium.svg
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
    install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(FILES third_party/lucide/LICENSE
            DESTINATION ${CMAKE_INSTALL_DOCDIR}/lucide)
endif()

set(CPACK_PACKAGE_NAME "gitmycelium")
set(CPACK_PACKAGE_VENDOR "GitMycelium contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Native Git client: commit graph, branch management and a merge editor")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "GitMycelium contributors")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_PACKAGE_FILE_NAME
    "gitmycelium-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

if(APPLE)
    # A .dmg holding the .app. The bundle is signed ad-hoc *before* cpack runs, and a
    # signature covers the bytes it was made over, so nothing may rewrite the binary
    # afterwards — hence no stripping.
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "GitMycelium")
    set(CPACK_DMG_FORMAT "UDBZ")
    set(CPACK_STRIP_FILES OFF)
else()
    # TGZ always works; DEB/RPM are one `cpack -G DEB|RPM` away where the tools exist.
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_STRIP_FILES ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgit2-1.9, libqt6widgets6 | libqt6widgets6t64")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_RPM_PACKAGE_REQUIRES "libgit2 >= 1.9, qt6-qtbase")
endif()
include(CPack)
