pybind11_add_module(bookreplay_py MODULE src/module.cpp)
set_target_properties(bookreplay_py PROPERTIES OUTPUT_NAME bookreplay)
target_link_libraries(bookreplay_py PRIVATE bookreplay::bookreplay)
bookreplay_set_warnings(bookreplay_py)

# FetchContent SYSTEM gives GCC and Clang -isystem; MSVC needs its own
# external-header level before it stops applying /W4 to pybind11.
target_compile_options(bookreplay_py PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/external:anglebrackets /external:W0>)

add_custom_command(TARGET bookreplay_py POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/bookreplay.pyi
          $<TARGET_FILE_DIR:bookreplay_py>/bookreplay.pyi)

if(BOOKREPLAY_BUILD_TESTS)
  add_test(NAME python_replay
    COMMAND ${Python_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  set_tests_properties(python_replay PROPERTIES
    LABELS python
    ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:bookreplay_py>")
endif()
