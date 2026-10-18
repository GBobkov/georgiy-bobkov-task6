#include "app.hpp"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: edition server [OPTIONS]\n"
                     "       edition client [HOST] [PORT]\n";
        return argc >= 2 ? 0 : 2;
    }

    try {
        const std::string_view mode(argv[1]);
        if (mode == "server") {
            return edition::run_server(argc - 1, argv + 1);
        }
        if (mode == "client") {
            return edition::run_client(argc - 1, argv + 1);
        }
        std::cerr << "Unknown mode: " << mode << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
