cmake_minimum_required(VERSION 3.20)

project(edition LANGUAGES CXX)

find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_executable(edition
    src/main.cpp
)

target_compile_features(edition PRIVATE cxx_std_20)
target_compile_options(edition PRIVATE -Wall -Wextra -Werror)
target_link_libraries(edition PRIVATE SQLite::SQLite3 OpenSSL::Crypto
                                      Threads::Threads)
