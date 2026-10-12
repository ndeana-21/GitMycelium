# Built, never added to ctest: a benchmark that asserts is a flaky test. Run it on
# a repository built by scripts/make-big-repo.sh.
add_executable(gitmycelium-bench bench_main.cpp)
target_link_libraries(gitmycelium-bench PRIVATE gmc_repo gmc_graph gmc_core)
