include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(bitkv_tests store_test.cpp)
target_link_libraries(bitkv_tests PRIVATE bitkv_core GTest::gtest_main)
bitkv_set_warnings(bitkv_tests)
bitkv_apply_sanitize(bitkv_tests)

include(GoogleTest)
gtest_discover_tests(bitkv_tests)
