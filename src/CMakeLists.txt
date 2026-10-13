add_library(gmc_core STATIC
    core/conflict_auto.cpp
    core/conflict_regions.cpp
    core/conflict_regions.h
    core/diff_rows.cpp
    core/diff_rows.h
    core/external_terminal.cpp
    core/external_terminal.h
    core/patch_selection.cpp
    core/porcelain_status.cpp
    core/porcelain_status.h
    core/patch_selection.h
    core/unified_diff.cpp
    core/unified_diff.h
    core/rebase_plan.cpp
    core/rebase_plan.h
    core/semantic_merge.cpp
    core/semantic_merge.h
    core/selection_plan.cpp
    core/selection_plan.h
    core/types.cpp
    core/types.h
)
target_include_directories(gmc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(gmc_git STATIC
    git/repository.cpp
    git/repository.h
    git/cli.cpp
    git/cli.h
    git/commit_graph.cpp
    git/commit_graph.h
    git/topo_walk.cpp
    git/topo_walk.h
    git/remote.cpp
    git/remote.h
    git/clone.cpp
    git/clone.h
    git/stash.cpp
    git/stash.h
    git/rerere.cpp
    git/rerere.h
    git/external_tools.cpp
    git/external_tools.h
    git/worktree.cpp
    git/worktree.h
    git/submodule.cpp
    git/submodule.h
    git/patch_ops.cpp
    git/patch_ops.h
    git/maintenance.cpp
    git/sparse.cpp
    git/gitflow.cpp
    git/lfs.cpp
    git/maintenance.h
    git/ref_meta.cpp
    git/ref_meta.h
    git/hooks.cpp
    git/hooks.h
)
target_link_libraries(gmc_git PUBLIC gmc_core PkgConfig::LIBGIT2)

add_library(gmc_repo STATIC
    repo/repo_session.cpp
    repo/repo_session.h
    repo/oplog.cpp
    repo/oplog.h
)
target_link_libraries(gmc_repo PUBLIC gmc_git Qt6::Core)

add_library(gmc_graph STATIC
    graph/graph_layout.cpp
    graph/graph_layout.h
    graph/graph_animation.cpp
    graph/graph_animation.h
)
target_link_libraries(gmc_graph PUBLIC gmc_core)

# Palette, stylesheet and glyphs. Its own library because both the models and the
# widgets need them: gmc_model cannot link gmc_ui without a cycle.
add_library(gmc_theme STATIC
    theme/theme.cpp
    theme/theme.h
    theme/glyphs.cpp
    theme/glyphs.h
    theme/icons.cpp
    theme/icons.h
)
target_include_directories(gmc_theme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gmc_theme PUBLIC Qt6::Gui Qt6::Svg)

add_library(gmc_model STATIC
    model/date_format.cpp
    model/date_format.h
    model/commit_model.cpp
    model/commit_model.h
    model/ref_model.cpp
    model/commit_files_model.cpp
    model/commit_files_model.h
    model/text_encoding.cpp
    model/text_encoding.h
    model/word_diff.cpp
    model/word_diff.h
    model/path_tree.cpp
    model/path_tree.h
    model/ref_pins.cpp
    model/ref_visibility.cpp
    model/ref_model.h
)
target_link_libraries(gmc_model PUBLIC gmc_core gmc_graph gmc_repo gmc_theme Qt6::Core Qt6::Gui)

add_library(gmc_update STATIC
    update/update_manifest.cpp
    update/update_manifest.h
    update/updater.cpp
    update/updater.h
)
target_include_directories(gmc_update PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gmc_update PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(gmc_update PRIVATE
    GMC_VERSION="${PROJECT_VERSION}" GMC_BUILD_NUMBER=${GMC_BUILD_NUMBER})

add_library(gmc_ui STATIC
    ui/diff_view.cpp
    ui/diff_view.h
    ui/diff_minimap.cpp
    ui/diff_minimap.h
    ui/commit_details_panel.cpp
    ui/commit_details_panel.h
    ui/working_tree_panel.cpp
    ui/working_tree_panel.h
    ui/grip_splitter.cpp
    ui/grip_splitter.h
    ui/file_history_dialog.cpp
    ui/reflog_dialog.cpp
    ui/reflog_dialog.h
    ui/sparse_checkout_dialog.cpp
    ui/sparse_checkout_dialog.h
    ui/gitflow_dialog.cpp
    ui/gitflow_dialog.h
    ui/lfs_dialog.cpp
    ui/lfs_dialog.h
    ui/profile_dialog.cpp
    ui/profile_dialog.h
    ui/profiles.cpp
    ui/profiles.h
    ui/notification_log.cpp
    ui/notification_log.h
    ui/file_history_dialog.h
    ui/rebase_editor.cpp
    ui/rebase_editor.h
    ui/repo_watcher.cpp
    ui/repo_watcher.h
    ui/main_window.cpp
    ui/badge_flight.cpp
    ui/drop_point.cpp
    ui/graph_operation_panel.cpp
    ui/syntax_highlight.cpp
    ui/main_window.h
    ui/graph_delegate.cpp
    ui/graph_delegate.h
    ui/refs_delegate.cpp
    ui/refs_delegate.h
    ui/ref_pill_layout.cpp
    ui/ref_pill_layout.h
    ui/commit_graph_view.cpp
    ui/commit_graph_view.h
    ui/command_palette.cpp
    ui/command_palette.h
    ui/ref_tree_view.cpp
    ui/ref_tree_view.h
    ui/stash_dialog.cpp
    ui/stash_dialog.h
    ui/worktree_dialog.cpp
    ui/worktree_dialog.h
    ui/submodule_dialog.cpp
    ui/submodule_dialog.h
    ui/conflict_view.cpp
    ui/conflict_view.h
    ui/settings_store.cpp
    ui/settings_store.h
    ui/clone_dialog.cpp
    ui/clone_dialog.h
    ui/coauthor_dialog.cpp
    ui/coauthor_dialog.h
    ui/theme_apply.cpp
    ui/theme_apply.h
    ui/themed_icons.cpp
    ui/themed_icons.h
    ui/keymap.cpp
    ui/keymap.h
    ui/search_panel.cpp
    ui/search_panel.h
    ui/preferences_dialog.cpp
    ui/preferences_dialog.h
    ui/recent_repositories.cpp
    ui/recent_repositories.h
    ui/start_page.cpp
    ui/start_page.h
    ui/create_repository_dialog.cpp
    ui/create_repository_dialog.h
    ui/hooks_dialog.cpp
    ui/hooks_dialog.h
    ui/update_dialog.cpp
    ui/update_dialog.h
)
target_link_libraries(gmc_ui PUBLIC gmc_model gmc_repo gmc_theme gmc_update Qt6::Widgets)

add_executable(gitmycelium app/main.cpp theme/icons.qrc
    ${CMAKE_SOURCE_DIR}/packaging/app_icon.qrc)
target_link_libraries(gitmycelium PRIVATE gmc_ui gmc_repo gmc_git gmc_core Qt6::Widgets Qt6::Svg)
# The version travels into the binary so --version and the About box report the
# same string CPack stamps into the package name.
target_compile_definitions(gitmycelium PRIVATE GMC_VERSION="${PROJECT_VERSION}")

if(APPLE)
    if(GMC_BUILD_NUMBER GREATER 0)
        set(GMC_BUNDLE_VERSION "${GMC_BUILD_NUMBER}")
    else()
        set(GMC_BUNDLE_VERSION "${PROJECT_VERSION}")
    endif()
    # A .app is what macOS runs. CPack's DragNDrop generator wraps this bundle, and
    # `macdeployqt` (run in CI) copies the Qt frameworks in beside the binary; without
    # it the app would only start on a machine that already has Qt. CMake writes the
    # Info.plist from its own template, so only the values differ.
    set_target_properties(gitmycelium PROPERTIES
        OUTPUT_NAME "GitMycelium"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "GitMycelium"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.gitmycelium.gitmycelium"
        MACOSX_BUNDLE_BUNDLE_VERSION "${GMC_BUNDLE_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "gitmycelium.icns"
        MACOSX_BUNDLE_COPYRIGHT "gitmycelium")
    # `iconutil` is macOS-only, so the .icns is built from the checked-in PNG in CI.
    # It is a bundle resource only when it is actually there, and it goes into
    # Contents/Resources by way of the package-location property — adding it as a
    # plain source would leave it nowhere near the icon the plist names.
    set(GMC_ICNS "${CMAKE_SOURCE_DIR}/packaging/icons/gitmycelium.icns")
    if(EXISTS "${GMC_ICNS}")
        target_sources(gitmycelium PRIVATE "${GMC_ICNS}")
        set_source_files_properties("${GMC_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    endif()
endif()
