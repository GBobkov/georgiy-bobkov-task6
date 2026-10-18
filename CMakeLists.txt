cmake_minimum_required(VERSION 3.20)

project(edition LANGUAGES CXX)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(edition_core
    src/client.cpp
    src/common.cpp
    src/net.cpp
    src/server.cpp
    src/server_connection.cpp
    src/store.cpp
    src/store_reporting.cpp
    src/store_sqlite.cpp
    src/store_support.cpp
    src/store_workflows.cpp
)

target_compile_features(edition_core PUBLIC cxx_std_20)
target_compile_options(edition_core PRIVATE -Wall -Wextra -Werror)
target_include_directories(edition_core PUBLIC include)
target_link_libraries(edition_core PUBLIC SQLite::SQLite3 OpenSSL::Crypto
                                           Threads::Threads)

add_executable(edition src/main.cpp)
target_compile_options(edition PRIVATE -Wall -Wextra -Werror)
target_link_libraries(edition PRIVATE edition_core)
