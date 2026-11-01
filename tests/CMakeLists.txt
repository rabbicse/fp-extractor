add_executable(fpx_tests
  test_main.cpp
  test_image.cpp
  test_template.cpp
  test_matcher.cpp
  test_gallery_store.cpp
  test_pipeline.cpp
  test_c_api.cpp)
target_link_libraries(fpx_tests PRIVATE fpx_core fpx doctest_header)
target_compile_definitions(fpx_tests PRIVATE FPX_TEST_TMP="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME fpx_tests COMMAND fpx_tests)

# CLI smoke test: the full demo must succeed.
add_test(NAME cli_demo COMMAND fpx_cli demo --out ${CMAKE_CURRENT_BINARY_DIR}/demo)
add_test(NAME cli_bench_small COMMAND fpx_cli bench --gallery 5000 --probes 5)
