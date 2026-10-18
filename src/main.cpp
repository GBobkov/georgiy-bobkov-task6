#include <exception>
#include <iostream>
#include <string_view>

#include "app.hpp"

namespace {

constexpr int kModeArgumentIndex = 1;
constexpr int kMinimumArgumentCount = 2;
constexpr int kInvalidUsageExitCode = 2;

}  // namespace

// Выбирает режим запуска; argc — число аргументов, argv — их массив.
// Ошибки запуска выводятся в stderr и преобразуются в ненулевой код выхода.
int main(int argc, char** argv) {
  if (argc < kMinimumArgumentCount ||
      std::string_view(argv[kModeArgumentIndex]) == "--help") {
    std::cout << "Usage: edition server [OPTIONS]\n"
                 "       edition client [HOST] [PORT]\n";
    return argc >= kMinimumArgumentCount ? 0 : kInvalidUsageExitCode;
  }

  try {
    const std::string_view mode(argv[kModeArgumentIndex]);
    if (mode == "server") {
      return edition::run_server(argc - 1, argv + 1);
    }
    if (mode == "client") {
      return edition::run_client(argc - 1, argv + 1);
    }
    std::cerr << "Unknown mode: " << mode << '\n';
    return kInvalidUsageExitCode;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
