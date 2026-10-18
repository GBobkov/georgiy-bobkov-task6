#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

#include "store_internal.hpp"

namespace edition::store_detail {
namespace {
constexpr int kInvalidHexDigit = -1;
constexpr int kCryptoSuccess = 1;
constexpr int kSecondHexCharacterOffset = 1;
constexpr int kHexDigitMask = 0x0f;
constexpr int kPasswordIterations = 240000;
constexpr int kPasswordSaltBytes = 16;
constexpr int kPasswordHashBytes = 32;
constexpr std::size_t kHexCharactersPerByte = 2;
constexpr int kBitsPerHexCharacter = 4;
constexpr int kHexadecimalDecimalBase = 10;
constexpr std::size_t kHexDigitCount = 16;
}  // namespace

// Кодирует массив байтов в шестнадцатеричную строку.
// data — начало массива; size — число байтов.
std::string hex_encode(const unsigned char* data, std::size_t size) {
  constexpr char digits[kHexDigitCount + 1] = "0123456789abcdef";
  std::string result(size * kHexCharactersPerByte, '0');
  for (std::size_t i = 0; i < size; ++i) {
    const std::size_t output_offset = i * kHexCharactersPerByte;
    result[output_offset] = digits[data[i] >> kBitsPerHexCharacter];
    result[output_offset + kSecondHexCharacterOffset] =
        digits[data[i] & kHexDigitMask];
  }
  return result;
}

// Преобразует шестнадцатеричный символ value в число 0–15 или возвращает -1.
int hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + kHexadecimalDecimalBase;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + kHexadecimalDecimalBase;
  }
  return kInvalidHexDigit;
}

// Декодирует шестнадцатеричную строку; value с неверным форматом даёт пустой
// результат.
std::vector<unsigned char> hex_decode(const std::string& value) {
  if (value.size() % kHexCharactersPerByte != 0) {
    return {};
  }
  std::vector<unsigned char> result(value.size() / kHexCharactersPerByte);
  for (std::size_t i = 0; i < result.size(); ++i) {
    const std::size_t input_offset = i * kHexCharactersPerByte;
    const int high = hex_value(value[input_offset]);
    const int low = hex_value(value[input_offset + kSecondHexCharacterOffset]);
    if (high < 0 || low < 0) {
      return {};
    }
    result[i] =
        static_cast<unsigned char>((high << kBitsPerHexCharacter) | low);
  }
  return result;
}

// Создаёт соль и PBKDF2-хеш пароля; password — исходный пароль.
// Возвращает только производные данные, исходный пароль не сохраняется.
PasswordDigest make_password_digest(const std::string& password) {
  std::array<unsigned char, kPasswordSaltBytes> salt{};
  std::array<unsigned char, kPasswordHashBytes> hash{};
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) !=
          kCryptoSuccess ||
      PKCS5_PBKDF2_HMAC(
          password.c_str(), static_cast<int>(password.size()), salt.data(),
          static_cast<int>(salt.size()), kPasswordIterations, EVP_sha256(),
          static_cast<int>(hash.size()), hash.data()) != kCryptoSuccess) {
    throw std::runtime_error("could not derive password hash");
  }
  return {hex_encode(salt.data(), salt.size()),
          hex_encode(hash.data(), hash.size())};
}

// Проверяет пароль постоянным по времени сравнением.
// password — введённый пароль; salt_hex и hash_hex — сохранённые значения.
// Возвращает false для повреждённых данных или несовпадения.
bool verify_password(const std::string& password, const std::string& salt_hex,
                     const std::string& hash_hex) {
  const std::vector<unsigned char> salt = hex_decode(salt_hex);
  const std::vector<unsigned char> expected = hex_decode(hash_hex);
  if (salt.size() != kPasswordSaltBytes ||
      expected.size() != kPasswordHashBytes) {
    return false;
  }
  std::array<unsigned char, kPasswordHashBytes> actual{};
  if (PKCS5_PBKDF2_HMAC(
          password.c_str(), static_cast<int>(password.size()), salt.data(),
          static_cast<int>(salt.size()), kPasswordIterations, EVP_sha256(),
          static_cast<int>(actual.size()), actual.data()) != kCryptoSuccess) {
    throw std::runtime_error("could not verify password");
  }
  return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

// Форматирует число фиксированной точностью.
// value — число; precision — количество знаков после запятой.
std::string format_number(double value, int precision) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(precision) << value;
  return output.str();
}

// Загружает настройки; database — соединение SQLite.
// Отсутствующие поля сохраняют значения по умолчанию из Settings.
Settings read_settings(sqlite3* database) {
  Settings result;
  Statement statement(database, "SELECT name, value FROM settings");
  while (statement.step() == SQLITE_ROW) {
    const std::string name = statement.text(kFirstColumn);
    if (name == "threshold") {
      result.threshold = statement.number(kSecondColumn);
    } else if (name == "half_life_seconds") {
      result.half_life_seconds =
          static_cast<std::int64_t>(statement.number(kSecondColumn));
    } else if (name == "congratulations_count") {
      result.congratulations_count =
          static_cast<std::int64_t>(statement.number(kSecondColumn));
    }
  }
  return result;
}

// Собирает аккаунт из текущей строки.
// statement — строка с колонками id, login, role и trust именно в этом порядке.
Account read_account(Statement& statement) {
  return {statement.integer(kFirstColumn), statement.text(kSecondColumn),
          statement.text(kThirdColumn), statement.number(kFourthColumn)};
}

// Проверяет пароль допустимой длины в байтах без управляющих символов.
// password — открытое значение до хеширования; неверный ввод вызывает
// исключение.
void validate_password(const std::string& password) {
  if (password.size() < limits::kMinimumPasswordLength ||
      !is_safe_field(password, limits::kMaximumPasswordLength)) {
    throw std::invalid_argument(
        "password must have " + std::to_string(limits::kMinimumPasswordLength) +
        " to " + std::to_string(limits::kMaximumPasswordLength) +
        " printable characters");
  }
}

// Проверяет логин допустимой длины в байтах без управляющих символов.
// login — имя учётной записи; неверный ввод вызывает исключение.
void validate_login(const std::string& login) {
  if (!is_safe_field(login, limits::kMaximumLoginLength)) {
    throw std::invalid_argument(
        "login must have " + std::to_string(limits::kMinimumLoginLength) +
        " to " + std::to_string(limits::kMaximumLoginLength) + " characters");
  }
}

}  // namespace edition::store_detail
