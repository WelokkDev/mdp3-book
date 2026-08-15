# databento-cpp is a development dependency and never part of the shipping
# library. It expects to be found already installed rather than built here:
# it pulls in OpenSSL 3, cpp-httplib, nlohmann_json and date, and documents
# only Ubuntu and macOS builds, so it lives in one Linux CI job instead of in
# everyone's working tree.
find_package(databento REQUIRED)

add_executable(oracle_diff oracle_diff.cpp)
target_link_libraries(oracle_diff PRIVATE bookreplay::bookreplay databento::databento)

# Deliberately not under bookreplay_set_warnings: this target compiles
# third-party headers we do not control, and a warning in them says nothing
# about whether the two decoders agree.
