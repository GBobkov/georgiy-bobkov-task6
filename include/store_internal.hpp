#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "store.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace edition::store_detail {

inline constexpr int kFirstColumn = 0;
inline constexpr int kSecondColumn = 1;
inline constexpr int kThirdColumn = 2;
inline constexpr int kFourthColumn = 3;
inline constexpr int kFifthColumn = 4;
inline constexpr int kSixthColumn = 5;
inline constexpr int kSeventhColumn = 6;
inline constexpr int kEighthColumn = 7;

inline constexpr int kFirstSqlParameter = 1;
inline constexpr int kSecondSqlParameter = 2;
inline constexpr int kThirdSqlParameter = 3;
inline constexpr int kFourthSqlParameter = 4;
inline constexpr int kFifthSqlParameter = 5;
inline constexpr int kSixthSqlParameter = 6;
inline constexpr int kSeventhSqlParameter = 7;
inline constexpr int kEighthSqlParameter = 8;
inline constexpr int kNinthSqlParameter = 9;
inline constexpr int kNullTerminatedSqlLength = -1;
inline constexpr int kDefaultNumberPrecision = 3;

std::runtime_error database_error(sqlite3* database, const std::string& action);

class Statement {
 public:
  Statement(sqlite3* database, const char* sql);
  ~Statement();

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind(int index, const std::string& value);
  void bind(int index, std::int64_t value);
  void bind(int index, double value);
  void bind_null(int index);
  int step();
  std::string text(int column) const;
  std::int64_t integer(int column) const;
  double number(int column) const;
  bool is_null(int column) const;

 private:
  sqlite3* database_ = nullptr;
  sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* database, const char* sql);

class Transaction {
 public:
  explicit Transaction(sqlite3* database);
  ~Transaction();

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void commit();

 private:
  sqlite3* database_ = nullptr;
  bool active_ = true;
};

struct PasswordDigest {
  std::string salt;
  std::string hash;
};

PasswordDigest make_password_digest(const std::string& password);
bool verify_password(const std::string& password, const std::string& salt_hex,
                     const std::string& hash_hex);
std::string format_number(double value,
                          int precision = kDefaultNumberPrecision);
Settings read_settings(sqlite3* database);
Account read_account(Statement& statement);
void validate_password(const std::string& password);
void validate_login(const std::string& login);

}  // namespace edition::store_detail
