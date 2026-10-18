#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: edition server|client\n";
        return argc == 2 ? 0 : 2;
    }

    std::cerr << "The requested mode is not implemented yet.\n";
    return 2;
}
