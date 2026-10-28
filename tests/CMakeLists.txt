add_executable(dws_tests
  test_http_parser.cpp
  test_metadata_dao.cpp
  test_router.cpp
  test_dao_contract.cpp
  test_qido_cache.cpp
  test_metrics.cpp
  test_circuit_breaker.cpp
)

target_link_libraries(dws_tests PRIVATE dws_core GTest::gtest GTest::gtest_main)

target_include_directories(dws_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/include")

include(GoogleTest)
gtest_discover_tests(dws_tests)
