include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  GIT_SHALLOW    TRUE
  SYSTEM                     # keep our -Werror off GoogleTest's headers
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # match MSVC's /MD default
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(bookreplay_tests
  test_dbn_layout.cpp
  test_invariants.cpp
)

target_include_directories(bookreplay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bookreplay_tests PRIVATE bookreplay::bookreplay GTest::gtest_main)
bookreplay_set_warnings(bookreplay_tests)

include(GoogleTest)
gtest_discover_tests(bookreplay_tests
  PROPERTIES LABELS unit
  DISCOVERY_TIMEOUT 60
)
