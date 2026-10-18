#include <sqlite3.h>

#include "store_internal.hpp"

namespace edition::store_detail {

std::runtime_error database_error(sqlite3* database,
                                  const std::string& action) {
  return std::runtime_error(action + ": " + sqlite3_errmsg(database));
}

Statement::Statement(sqlite3* database, const char* sql) : database_(database) {
  if (sqlite3_prepare_v2(database_, sql, kNullTerminatedSqlLength, &statement_,
                         nullptr) != SQLITE_OK) {
    throw database_error(database_, "cannot prepare SQL statement");
  }
}

Statement::~Statement() { sqlite3_finalize(statement_); }

void Statement::bind(int index, const std::string& value) {
  if (sqlite3_bind_text(statement_, index, value.c_str(),
                        static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    throw database_error(database_, "cannot bind text value");
  }
}

void Statement::bind(int index, std::int64_t value) {
  if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
    throw database_error(database_, "cannot bind integer value");
  }
}

void Statement::bind(int index, double value) {
  if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK) {
    throw database_error(database_, "cannot bind numeric value");
  }
}

void Statement::bind_null(int index) {
  if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
    throw database_error(database_, "cannot bind null value");
  }
}

int Statement::step() {
  const int result = sqlite3_step(statement_);
  if (result != SQLITE_ROW && result != SQLITE_DONE) {
    throw database_error(database_, "SQL statement failed");
  }
  return result;
}

std::string Statement::text(int column) const {
  const auto* value = sqlite3_column_text(statement_, column);
  return value == nullptr ? std::string{}
                          : reinterpret_cast<const char*>(value);
}

std::int64_t Statement::integer(int column) const {
  return sqlite3_column_int64(statement_, column);
}

double Statement::number(int column) const {
  return sqlite3_column_double(statement_, column);
}

bool Statement::is_null(int column) const {
  return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

void execute(sqlite3* database, const char* sql) {
  char* error = nullptr;
  const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string message =
        error == nullptr ? sqlite3_errmsg(database) : error;
    sqlite3_free(error);
    throw std::runtime_error("SQL operation failed: " + message);
  }
}

Transaction::Transaction(sqlite3* database) : database_(database) {
  execute(database_, "BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
  if (active_) {
    sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
  }
}

void Transaction::commit() {
  execute(database_, "COMMIT");
  active_ = false;
}

}  // namespace edition::store_detail
