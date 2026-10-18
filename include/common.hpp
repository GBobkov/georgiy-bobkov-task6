#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace edition {

namespace limits {
inline constexpr std::size_t kMinimumPasswordLength = 8;
inline constexpr std::size_t kMaximumPasswordLength = 1024;
inline constexpr std::size_t kMinimumLoginLength = 1;
inline constexpr std::size_t kMaximumLoginLength = 128;
inline constexpr std::size_t kMaximumRequestIdLength = 128;
inline constexpr std::size_t kMaximumCategoryLength = 128;
inline constexpr std::size_t kMaximumTopicLength = 1024;
}  // namespace limits

// Общие типы сетевого протокола и разбора данных для сервера и клиента.
using Tokens = std::vector<std::string>;
using Rows = std::vector<Tokens>;

// Разбирает строку протокола; line — строка без перевода строки.
// Возвращает токены или бросает исключение при неверном формате кавычек.
Tokens parse_tokens(const std::string& line);

// Кодирует поля в строку протокола и добавляет перевод строки.
// tokens — поля сообщения.
std::string encode_tokens(const Tokens& tokens);

// Склеивает значения для отображения, не для передачи по сети.
// tokens — значения; separator — разделитель между ними.
std::string join_tokens(const Tokens& tokens, std::string_view separator);

// Разбирает целое число; value — строковое представление без пробелов.
// Неверный ввод или лишние символы вызывают исключение.
std::int64_t parse_integer(const std::string& value);

// Разбирает конечное число; value — строковое представление без остатка.
// Неверный ввод вызывает исключение.
double parse_number(const std::string& value);

// Разбирает реальную дату UTC формата YYYY-MM-DDTHH:MM:SSZ в Unix-секунды.
// value — строковая дата; несуществующая дата вызывает исключение.
std::int64_t parse_utc_time(const std::string& value);

// Форматирует Unix-время в UTC; value — число секунд с начала эпохи.
// При невозможности форматирования возвращает специальную метку.
std::string format_utc_time(std::int64_t value);

// Возвращает текущее время в Unix-секундах.
std::int64_t current_time();

// Запрашивает секрет; prompt — приглашение, ввод скрывается только в терминале.
std::string prompt_secret(const std::string& prompt);

// Проверяет непустое поле без управляющих символов.
// value — поле; max_length — его предел в байтах.
// Возвращает true только для допустимого значения.
bool is_safe_field(const std::string& value, std::size_t max_length);

}  // namespace edition
