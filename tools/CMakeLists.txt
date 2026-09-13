add_executable(dbn_dump dbn_dump.cpp)
target_link_libraries(dbn_dump PRIVATE bookreplay::bookreplay)
bookreplay_set_warnings(dbn_dump)

add_executable(replay_demo replay_demo.cpp)
target_link_libraries(replay_demo PRIVATE bookreplay::bookreplay)
bookreplay_set_warnings(replay_demo)

add_executable(book_check book_check.cpp)
target_link_libraries(book_check PRIVATE bookreplay::bookreplay)
bookreplay_set_warnings(book_check)
