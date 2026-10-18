#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace edition {

// Shared wire-format and parsing types used by both console modes.
using Tokens = std::vector<std::string>;
using Rows = std::vector<Tokens>;

Tokens parse_tokens(const std::string& line);
std::string encode_tokens(const Tokens& tokens);
std::string join_tokens(const Tokens& tokens, std::string_view separator);

std::int64_t parse_integer(const std::string& value);
double parse_number(const std::string& value);
std::int64_t parse_utc_time(const std::string& value);
std::string format_utc_time(std::int64_t value);
std::int64_t current_time();
std::string prompt_secret(const std::string& prompt);

bool is_safe_field(const std::string& value, std::size_t max_length);

}  // namespace edition
