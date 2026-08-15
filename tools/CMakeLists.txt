add_executable(dbn_dump dbn_dump.cpp)
target_link_libraries(dbn_dump PRIVATE bookreplay::bookreplay)
bookreplay_set_warnings(dbn_dump)
