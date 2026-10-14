cmake_minimum_required(VERSION 3.21)

project(gitmycelium VERSION 1.2.0
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
# libgit2 comes from pkg-config where there is one; Windows has none, so there it is
# found by its header and import library (LIBGIT2_ROOT, or CMAKE_PREFIX_PATH).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32)
    pkg_check_modules(LIBGIT2 QUIET IMPORTED_TARGET libgit2)
endif()
add_library(gmc_libgit2 INTERFACE)
if(TARGET PkgConfig::LIBGIT2)
    target_link_libraries(gmc_libgit2 INTERFACE PkgConfig::LIBGIT2)
else()
    find_path(LIBGIT2_INCLUDE_DIR git2.h HINTS "${LIBGIT2_ROOT}/include" REQUIRED)
    find_library(LIBGIT2_LIBRARY NAMES git2 libgit2 HINTS "${LIBGIT2_ROOT}/lib" REQUIRED)
    target_include_directories(gmc_libgit2 INTERFACE "${LIBGIT2_INCLUDE_DIR}")
    target_link_libraries(gmc_libgit2 INTERFACE "${LIBGIT2_LIBRARY}")
    message(STATUS "libgit2: ${LIBGIT2_LIBRARY}")
endif()

if(MSVC)
    # Sources are UTF-8; `__cplusplus` must say what the standard really is.
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /EHsc)
else()
    add_compile_options(-Wall -Wextra)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    # Every executable runs with UTF-8 as its ANSI code page: paths travel through the
    # git layer as UTF-8 `std::string`s, and `std::filesystem`, the C runtime and
    # libgit2 then agree on what they mean (a repository under a Cyrillic folder).
    set(GMC_WINDOWS_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/gitmycelium.manifest")
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
elseif(WIN32)
    # A folder that runs where it lands: windeployqt (run in CI) puts Qt beside the
    # executable, and git2.dll travels with it.
    install(TARGETS gitmycelium RUNTIME DESTINATION .)
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
    # The license travels in Licenses/. As a click-through agreement it also made
    # `hdiutil attach` stop and wait for "Y", which hung the in-app update.
    set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
    set(CPACK_STRIP_FILES OFF)
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
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
