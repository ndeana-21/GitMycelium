add_executable(test_git_repository test_git_repository.cpp)
target_link_libraries(test_git_repository PRIVATE gmc_git gmc_repo Qt6::Test Qt6::Core)

add_executable(test_graph_layout test_graph_layout.cpp)
target_link_libraries(test_graph_layout PRIVATE gmc_graph Qt6::Test Qt6::Core)

add_executable(test_merge_workflow test_merge_workflow.cpp)
target_link_libraries(test_merge_workflow PRIVATE gmc_git Qt6::Test Qt6::Core)

add_executable(test_rebase_and_sync test_rebase_and_sync.cpp)
target_link_libraries(test_rebase_and_sync PRIVATE gmc_git Qt6::Test Qt6::Core)

add_executable(test_badge_flight test_badge_flight.cpp)
target_link_libraries(test_badge_flight PRIVATE gmc_ui Qt6::Test Qt6::Gui Qt6::Widgets)

add_executable(test_syntax_highlight test_syntax_highlight.cpp)
target_link_libraries(test_syntax_highlight PRIVATE gmc_ui Qt6::Test Qt6::Gui Qt6::Widgets)

add_executable(test_ref_dnd test_ref_dnd.cpp)
target_link_libraries(test_ref_dnd PRIVATE gmc_model gmc_ui Qt6::Test Qt6::Gui Qt6::Widgets)

add_executable(test_graph_refs test_graph_refs.cpp)
target_link_libraries(test_graph_refs PRIVATE gmc_graph gmc_ui Qt6::Test Qt6::Gui Qt6::Widgets)

add_executable(test_history_ops test_history_ops.cpp)
target_link_libraries(test_history_ops PRIVATE gmc_git Qt6::Test Qt6::Core)

add_test(NAME git_repository COMMAND test_git_repository)
add_test(NAME graph_layout COMMAND test_graph_layout)
add_test(NAME merge_workflow COMMAND test_merge_workflow)
add_test(NAME rebase_and_sync COMMAND test_rebase_and_sync)
add_test(NAME badge_flight COMMAND test_badge_flight)
add_test(NAME syntax_highlight COMMAND test_syntax_highlight)
add_test(NAME ref_dnd COMMAND test_ref_dnd)
add_test(NAME graph_refs COMMAND test_graph_refs)
add_test(NAME history_ops COMMAND test_history_ops)

add_executable(test_ref_tree test_ref_tree.cpp)
target_link_libraries(test_ref_tree PRIVATE gmc_model gmc_theme Qt6::Gui Qt6::Test)
add_test(NAME ref_tree COMMAND test_ref_tree)

add_executable(test_commit_files test_commit_files.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_commit_files PRIVATE gmc_model gmc_theme Qt6::Gui Qt6::Test)
add_test(NAME commit_files COMMAND test_commit_files)

add_executable(test_text_tools test_text_tools.cpp)
target_link_libraries(test_text_tools PRIVATE gmc_model gmc_theme Qt6::Gui Qt6::Test)
add_test(NAME text_tools COMMAND test_text_tools)

add_executable(test_word_diff test_word_diff.cpp)
target_link_libraries(test_word_diff PRIVATE gmc_model Qt6::Gui Qt6::Test)
add_test(NAME word_diff COMMAND test_word_diff)

add_executable(test_diff_view test_diff_view.cpp)
target_link_libraries(test_diff_view PRIVATE gmc_ui gmc_model gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME diff_view COMMAND test_diff_view)

add_executable(test_theme_icons test_theme_icons.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_theme_icons PRIVATE gmc_theme Qt6::Gui Qt6::Svg Qt6::Test)
add_test(NAME theme_icons COMMAND test_theme_icons)

add_executable(test_theme test_theme.cpp)
target_link_libraries(test_theme PRIVATE gmc_theme Qt6::Gui Qt6::Test)
add_test(NAME theme COMMAND test_theme)

add_executable(test_themed_icons test_themed_icons.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_themed_icons PRIVATE gmc_ui gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME themed_icons COMMAND test_themed_icons)

add_executable(test_panels test_panels.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_panels PRIVATE gmc_ui gmc_model gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME panels COMMAND test_panels)

add_executable(test_conflict_auto test_conflict_auto.cpp)
target_link_libraries(test_conflict_auto PRIVATE gmc_core Qt6::Test)
add_test(NAME test_conflict_auto COMMAND test_conflict_auto)

add_executable(test_conflict_regions test_conflict_regions.cpp)
target_link_libraries(test_conflict_regions PRIVATE gmc_core Qt6::Test)
add_test(NAME test_conflict_regions COMMAND test_conflict_regions)

add_executable(test_conflict_view test_conflict_view.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_conflict_view PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_conflict_view COMMAND test_conflict_view)

add_executable(test_conflict_binary test_conflict_binary.cpp)
target_link_libraries(test_conflict_binary PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_conflict_binary COMMAND test_conflict_binary)

add_executable(test_merge_preview test_merge_preview.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_merge_preview PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_merge_preview COMMAND test_merge_preview)

add_executable(test_rerere test_rerere.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_rerere PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_rerere COMMAND test_rerere)

add_executable(test_squash_merge test_squash_merge.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_squash_merge PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_squash_merge COMMAND test_squash_merge)

add_executable(test_graph_visibility test_graph_visibility.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_graph_visibility PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_graph_visibility COMMAND test_graph_visibility)

add_executable(test_smart_visibility test_smart_visibility.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_smart_visibility PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_smart_visibility COMMAND test_smart_visibility)

add_executable(test_graph_hover test_graph_hover.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_graph_hover PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_graph_hover COMMAND test_graph_hover)


add_executable(test_graph_columns test_graph_columns.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_graph_columns PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_graph_columns COMMAND test_graph_columns)

add_executable(test_timeline test_timeline.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_timeline PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_timeline COMMAND test_timeline)

add_executable(test_worktrees test_worktrees.cpp)
target_link_libraries(test_worktrees PRIVATE gmc_repo gmc_git gmc_core Qt6::Test)
add_test(NAME test_worktrees COMMAND test_worktrees)

add_executable(test_submodules test_submodules.cpp)
target_link_libraries(test_submodules PRIVATE gmc_repo gmc_git gmc_core Qt6::Test)
add_test(NAME test_submodules COMMAND test_submodules)

add_executable(test_conflict_sweep test_conflict_sweep.cpp)
target_link_libraries(test_conflict_sweep PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_conflict_sweep COMMAND test_conflict_sweep)

add_executable(test_branch_ops test_branch_ops.cpp)
target_link_libraries(test_branch_ops PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_branch_ops COMMAND test_branch_ops)

add_executable(test_selection_plan test_selection_plan.cpp)
target_link_libraries(test_selection_plan PRIVATE gmc_core Qt6::Test)
add_test(NAME test_selection_plan COMMAND test_selection_plan)

add_executable(test_graph_selection test_graph_selection.cpp)
target_link_libraries(test_graph_selection PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME test_graph_selection COMMAND test_graph_selection)

add_executable(test_wip_node test_wip_node.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_wip_node PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME wip_node COMMAND test_wip_node)

add_executable(test_command_palette test_command_palette.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_command_palette PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME command_palette COMMAND test_command_palette)

add_executable(test_preferences test_preferences.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_preferences PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME preferences COMMAND test_preferences)

add_executable(test_keymap test_keymap.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_keymap PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME keymap COMMAND test_keymap)

add_executable(test_commit_search test_commit_search.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_commit_search PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME commit_search COMMAND test_commit_search)

add_executable(test_oplog test_oplog.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_oplog PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME oplog COMMAND test_oplog)

add_executable(test_staging test_staging.cpp)
target_link_libraries(test_staging PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_staging COMMAND test_staging)

add_executable(test_staging_lines test_staging_lines.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_staging_lines PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_staging_lines COMMAND test_staging_lines)

add_executable(test_stash test_stash.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_stash PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_stash COMMAND test_stash)

add_executable(test_remotes test_remotes.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_remotes PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_remotes COMMAND test_remotes)

add_executable(test_conflict_prevention test_conflict_prevention.cpp)
target_link_libraries(test_conflict_prevention PRIVATE gmc_git gmc_repo gmc_core Qt6::Test Qt6::Core)
add_test(NAME test_conflict_prevention COMMAND test_conflict_prevention)

add_executable(test_reflog test_reflog.cpp)
target_link_libraries(test_reflog PRIVATE gmc_git gmc_repo gmc_core Qt6::Test Qt6::Core)
add_test(NAME test_reflog COMMAND test_reflog)

add_executable(test_sparse test_sparse.cpp)
target_link_libraries(test_sparse PRIVATE gmc_git gmc_repo gmc_core Qt6::Test Qt6::Core)
add_test(NAME test_sparse COMMAND test_sparse)

add_executable(test_profiles test_profiles.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_profiles PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_profiles COMMAND test_profiles)

add_executable(test_gitflow test_gitflow.cpp)
target_link_libraries(test_gitflow PRIVATE gmc_git gmc_repo gmc_core Qt6::Test Qt6::Core)
add_test(NAME test_gitflow COMMAND test_gitflow)

add_executable(test_notifications test_notifications.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_notifications PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_notifications COMMAND test_notifications)

add_executable(test_lfs test_lfs.cpp)
target_link_libraries(test_lfs PRIVATE gmc_git gmc_repo gmc_core Qt6::Test Qt6::Core)
add_test(NAME test_lfs COMMAND test_lfs)

add_executable(test_date_format test_date_format.cpp)
target_link_libraries(test_date_format PRIVATE gmc_model gmc_repo gmc_git gmc_core Qt6::Test)
add_test(NAME test_date_format COMMAND test_date_format)

add_executable(test_ref_tracking test_ref_tracking.cpp)
target_link_libraries(test_ref_tracking PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
                                              Qt6::Widgets Qt6::Test)
add_test(NAME test_ref_tracking COMMAND test_ref_tracking)

add_executable(test_graph_view_bind test_graph_view_bind.cpp)
target_link_libraries(test_graph_view_bind PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core
                                                  gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME test_graph_view_bind COMMAND test_graph_view_bind)

add_executable(test_graph_paging test_graph_paging.cpp)
target_link_libraries(test_graph_paging PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core
                                               gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME test_graph_paging COMMAND test_graph_paging)

add_executable(test_graph_animation test_graph_animation.cpp)
target_link_libraries(test_graph_animation PRIVATE gmc_graph gmc_core Qt6::Test)
add_test(NAME test_graph_animation COMMAND test_graph_animation)

add_executable(test_branch_group test_branch_group.cpp)
target_link_libraries(test_branch_group PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_branch_group COMMAND test_branch_group)

add_executable(test_patch_ops test_patch_ops.cpp)
target_link_libraries(test_patch_ops PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_patch_ops COMMAND test_patch_ops)

add_executable(test_maintenance test_maintenance.cpp)
target_link_libraries(test_maintenance PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_maintenance COMMAND test_maintenance)

add_executable(test_history_paging test_history_paging.cpp)
target_link_libraries(test_history_paging PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_history_paging COMMAND test_history_paging)

add_executable(test_topo_walk test_topo_walk.cpp)
target_link_libraries(test_topo_walk PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_topo_walk COMMAND test_topo_walk)

add_executable(test_status test_status.cpp)
target_link_libraries(test_status PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_status COMMAND test_status)

add_executable(test_filters test_filters.cpp)
target_link_libraries(test_filters PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_filters COMMAND test_filters)

add_executable(test_repo_watcher test_repo_watcher.cpp)
target_link_libraries(test_repo_watcher PRIVATE gmc_ui Qt6::Widgets Qt6::Test)
add_test(NAME test_repo_watcher COMMAND test_repo_watcher)

add_executable(test_interactive_rebase test_interactive_rebase.cpp)
target_link_libraries(test_interactive_rebase PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_interactive_rebase COMMAND test_interactive_rebase)

add_executable(test_rebase_plan test_rebase_plan.cpp)
target_link_libraries(test_rebase_plan PRIVATE gmc_core Qt6::Test)
add_test(NAME test_rebase_plan COMMAND test_rebase_plan)

add_executable(test_diff_rows test_diff_rows.cpp)
target_link_libraries(test_diff_rows PRIVATE gmc_core Qt6::Test)
add_test(NAME test_diff_rows COMMAND test_diff_rows)

add_executable(test_semantic_merge test_semantic_merge.cpp)
target_link_libraries(test_semantic_merge PRIVATE gmc_core Qt6::Test)
add_test(NAME test_semantic_merge COMMAND test_semantic_merge)

add_executable(test_diff_between test_diff_between.cpp)
target_link_libraries(test_diff_between PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_diff_between COMMAND test_diff_between)

add_executable(test_blame test_blame.cpp)
target_link_libraries(test_blame PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_blame COMMAND test_blame)

add_executable(test_rebase_editor test_rebase_editor.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_rebase_editor PRIVATE gmc_ui gmc_model gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME test_rebase_editor COMMAND test_rebase_editor)

add_executable(test_partial_clone test_partial_clone.cpp)
target_link_libraries(test_partial_clone PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_partial_clone COMMAND test_partial_clone)

add_executable(test_clone test_clone.cpp)
target_link_libraries(test_clone PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_clone COMMAND test_clone)

add_executable(test_commit_hygiene test_commit_hygiene.cpp)
target_link_libraries(test_commit_hygiene PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_commit_hygiene COMMAND test_commit_hygiene)

add_executable(test_compare test_compare.cpp)
target_link_libraries(test_compare PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_compare COMMAND test_compare)

add_executable(test_tabs test_tabs.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_tabs PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme Qt6::Widgets Qt6::Test)
add_test(NAME test_tabs COMMAND test_tabs)

add_executable(test_recent_repositories test_recent_repositories.cpp)
target_link_libraries(test_recent_repositories PRIVATE gmc_ui Qt6::Test Qt6::Widgets)
add_test(NAME test_recent_repositories COMMAND test_recent_repositories)

add_executable(test_start_page test_start_page.cpp)
target_link_libraries(test_start_page PRIVATE gmc_ui Qt6::Test Qt6::Widgets)
add_test(NAME test_start_page COMMAND test_start_page)

add_executable(test_create_repository test_create_repository.cpp)
target_link_libraries(test_create_repository PRIVATE gmc_repo gmc_git Qt6::Test Qt6::Core)
add_test(NAME test_create_repository COMMAND test_create_repository)

add_executable(test_external_tools test_external_tools.cpp)
target_link_libraries(test_external_tools PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_external_tools COMMAND test_external_tools)

add_executable(test_external_terminal test_external_terminal.cpp)
target_link_libraries(test_external_terminal PRIVATE gmc_core Qt6::Test)
add_test(NAME test_external_terminal COMMAND test_external_terminal)

add_executable(test_unified_diff test_unified_diff.cpp)
target_link_libraries(test_unified_diff PRIVATE gmc_git gmc_repo gmc_core Qt6::Test)
add_test(NAME test_unified_diff COMMAND test_unified_diff)

add_executable(test_hooks test_hooks.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_hooks PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_hooks COMMAND test_hooks)

add_executable(test_signing test_signing.cpp ${CMAKE_SOURCE_DIR}/src/theme/icons.qrc)
target_link_libraries(test_signing PRIVATE gmc_ui gmc_model gmc_repo gmc_git gmc_core gmc_theme
    Qt6::Widgets Qt6::Test)
add_test(NAME test_signing COMMAND test_signing)

add_executable(test_cli test_cli.cpp)
target_link_libraries(test_cli PRIVATE gmc_git Qt6::Test Qt6::Core)
add_test(NAME test_cli COMMAND test_cli)

add_executable(test_commit_paging test_commit_paging.cpp)
target_link_libraries(test_commit_paging PRIVATE gmc_model gmc_repo gmc_git gmc_core gmc_theme
                                                 Qt6::Gui Qt6::Test)
add_test(NAME test_commit_paging COMMAND test_commit_paging)

add_executable(test_updater test_updater.cpp)
target_link_libraries(test_updater PRIVATE gmc_update Qt6::Test Qt6::Core)
add_test(NAME updater COMMAND test_updater)

add_executable(test_update_dialog test_update_dialog.cpp)
target_link_libraries(test_update_dialog PRIVATE gmc_ui Qt6::Test Qt6::Widgets)
add_test(NAME update_dialog COMMAND test_update_dialog)


# A test's outcome must not depend on the machine. git refuses to commit without an
# identity and a fresh CI runner has none, while a developer's global settings
# (signing, hooks path) would leak in, so every test reads this file as its global
# config; repository config and `-c` still override it, which the authorship tests
# rely on. The system config is ignored too: a runner with git-lfs installed has
# its filter in /etc/gitconfig. A runner has no display either, so widgets render
# offscreen.
set(GMC_TEST_GITCONFIG "${CMAKE_CURRENT_BINARY_DIR}/gitconfig")
file(WRITE "${GMC_TEST_GITCONFIG}"
     "[user]\n\tname = GitMycelium Tests\n\temail = tests@gitmycelium.invalid\n")
get_property(GMC_ALL_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${GMC_ALL_TESTS} PROPERTIES
                     ENVIRONMENT "GIT_CONFIG_GLOBAL=${GMC_TEST_GITCONFIG};GIT_CONFIG_NOSYSTEM=1;QT_QPA_PLATFORM=offscreen")
