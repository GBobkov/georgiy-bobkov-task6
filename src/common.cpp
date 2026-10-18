#include "common.hpp"

#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <charconv>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace edition {
namespace {

constexpr std::size_t kUtcTimestampLength = 20;
constexpr std::size_t kUtcTimestampPrefixLength = kUtcTimestampLength - 1;
constexpr std::size_t kUtcTimezoneSuffixIndex = kUtcTimestampPrefixLength;
constexpr std::size_t kStringTerminatorLength = 1;
constexpr std::size_t kUtcTimestampBufferLength =
    kUtcTimestampLength + kStringTerminatorLength;
constexpr unsigned char kFirstPrintableAscii = 0x20;
constexpr unsigned char kAsciiDelete = 0x7f;

}  // namespace

// Разбирает строку протокола с кавычками и экранированием; line — входная
// строка без завершающего перевода строки.
Tokens parse_tokens(const std::string& line) {
  Tokens tokens;
  std::size_t position = 0;
  while (position < line.size()) {
    while (position < line.size() &&
           std::isspace(static_cast<unsigned char>(line[position])) != 0) {
      ++position;
    }
    if (position == line.size()) {
      break;
    }

    std::string token;
    if (line[position] == '"') {
      // Кавычки сохраняют пробелы в поле, а обратная косая черта
      // экранирует следующий символ.
      ++position;
      bool closed = false;
      while (position < line.size()) {
        const char character = line[position++];
        if (character == '"') {
          closed = true;
          break;
        }
        if (character == '\\') {
          if (position == line.size()) {
            throw std::invalid_argument("invalid quoted command");
          }
          token.push_back(line[position++]);
        } else {
          token.push_back(character);
        }
      }
      if (!closed ||
          (position < line.size() &&
           std::isspace(static_cast<unsigned char>(line[position])) == 0)) {
        throw std::invalid_argument("invalid quoted command");
      }
    } else {
      while (position < line.size() &&
             std::isspace(static_cast<unsigned char>(line[position])) == 0) {
        token.push_back(line[position++]);
      }
    }
    tokens.push_back(std::move(token));
  }
  return tokens;
}

// Кодирует поля одной строки протокола; tokens — последовательность полей.
// Кавычки вокруг каждого поля сохраняют границы при повторном разборе.
std::string encode_tokens(const Tokens& tokens) {
  std::ostringstream output;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0) {
      output << ' ';
    }
    output << std::quoted(tokens[i]);
  }
  output << '\n';
  return output.str();
}

// Объединяет поля разделителем; tokens — значения, separator — строка между
// соседними значениями.
std::string join_tokens(const Tokens& tokens, std::string_view separator) {
  std::string result;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0) {
      result += separator;
    }
    result += tokens[i];
  }
  return result;
}

// Преобразует value в целое число и отклоняет частичный или неверный разбор.
std::int64_t parse_integer(const std::string& value) {
  std::int64_t result = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto [position, error] = std::from_chars(begin, end, result);
  if (error != std::errc{} || position != end) {
    throw std::invalid_argument("expected an integer");
  }
  return result;
}

// Разбирает конечное вещественное число; value должен целиком содержать число.
double parse_number(const std::string& value) {
  std::size_t parsed = 0;
  double result = 0.0;
  try {
    result = std::stod(value, &parsed);
  } catch (const std::exception&) {
    throw std::invalid_argument("expected a number");
  }
  if (parsed != value.size() || !std::isfinite(result)) {
    throw std::invalid_argument("expected a finite number");
  }
  return result;
}

// Разбирает время UTC строго в формате YYYY-MM-DDTHH:MM:SSZ; value — строка
// времени. Возвращает секунды Unix и отклоняет даты, нормализуемые timegm.
std::int64_t parse_utc_time(const std::string& value) {
  if (value.size() != kUtcTimestampLength ||
      value[kUtcTimezoneSuffixIndex] != 'Z') {
    throw std::invalid_argument("time must use YYYY-MM-DDTHH:MM:SSZ");
  }

  std::tm parsed{};
  std::istringstream input(value.substr(0, kUtcTimestampPrefixLength));
  input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) {
    throw std::invalid_argument("invalid UTC time");
  }

  // timegm нормализует переполненные даты; обратное преобразование
  // позволяет обнаружить такую нормализацию и отклонить неверную дату.
  const std::tm original = parsed;
  const std::time_t timestamp = timegm(&parsed);
  std::tm normalized{};
  if (gmtime_r(&timestamp, &normalized) == nullptr ||
      normalized.tm_year != original.tm_year ||
      normalized.tm_mon != original.tm_mon ||
      normalized.tm_mday != original.tm_mday ||
      normalized.tm_hour != original.tm_hour ||
      normalized.tm_min != original.tm_min ||
      normalized.tm_sec != original.tm_sec) {
    throw std::invalid_argument("invalid UTC time");
  }
  return static_cast<std::int64_t>(timestamp);
}

// Форматирует секунды Unix в UTC; value — метка времени, для которой
// возвращается строка ISO 8601 либо invalid-time.
std::string format_utc_time(std::int64_t value) {
  const std::time_t timestamp = static_cast<std::time_t>(value);
  std::tm parts{};
  if (gmtime_r(&timestamp, &parts) == nullptr) {
    return "invalid-time";
  }

  char buffer[kUtcTimestampBufferLength]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts) ==
      0) {
    return "invalid-time";
  }
  return buffer;
}

// Возвращает текущее время в секундах Unix для отметок приёма и событий.
std::int64_t current_time() {
  return static_cast<std::int64_t>(std::time(nullptr));
}

// Считывает секрет из терминала; prompt — приглашение, выводимое до ввода.
// Эхо отключается только если stdin является терминалом.
std::string prompt_secret(const std::string& prompt) {
  std::cout << prompt << std::flush;
  termios previous{};
  const bool can_hide =
      isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &previous) == 0;
  if (can_hide) {
    // Эхо отключается только в терминале и восстанавливается после ввода.
    termios hidden = previous;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
      throw std::runtime_error("cannot disable terminal echo");
    }
  }

  std::string secret;
  std::getline(std::cin, secret);
  if (can_hide) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous);
    std::cout << '\n';
  }
  if (!std::cin) {
    throw std::runtime_error("could not read password");
  }
  return secret;
}

// Проверяет длину и отсутствие управляющих символов; max_length задан в байтах.
bool is_safe_field(const std::string& value, std::size_t max_length) {
  if (value.empty() || value.size() > max_length) {
    return false;
  }
  for (unsigned char character : value) {
    if (character < kFirstPrintableAscii || character == kAsciiDelete) {
      return false;
    }
  }
  return true;
}

}  // namespace edition
