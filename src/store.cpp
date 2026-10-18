#include <sqlite3.h>

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "store_internal.hpp"

namespace edition {
using namespace store_detail;
namespace {
constexpr int kMillisecondsPerSecond = 1000;
constexpr int kSqliteBusyTimeoutSeconds = 5;
constexpr double kSeedTrustForSugarissa = 1.0;
constexpr double kSeedTrustForSelectiveTony = 0.25;
constexpr double kSeedTrustForSometimesPugh = 0.35;
constexpr std::size_t kSeedClientCount = 3;
}  // namespace

// path — путь к файлу SQLite; открывает базу и подготавливает её схему.
Store::Store(const std::string& path) {
  if (sqlite3_open_v2(
          path.c_str(), &database_,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
          nullptr) != SQLITE_OK) {
    const std::string message = database_ == nullptr
                                    ? "could not open database"
                                    : sqlite3_errmsg(database_);
    if (database_ != nullptr) {
      sqlite3_close(database_);
      database_ = nullptr;
    }
    throw std::runtime_error(message);
  }
  sqlite3_busy_timeout(database_,
                       kSqliteBusyTimeoutSeconds * kMillisecondsPerSecond);
  execute(database_, "PRAGMA foreign_keys = ON");
  execute(database_, "PRAGMA journal_mode = WAL");
  initialize_schema();
}

// Закрывает соединение SQLite, которым владеет хранилище.
Store::~Store() {
  if (database_ != nullptr) {
    sqlite3_close(database_);
  }
}

// Создаёт таблицы и ограничения; недостающий столбец добавляется для старых БД.
void Store::initialize_schema() {
  // UNIQUE-ограничения защищают от повторных голосов и запросов даже при
  // одновременной записи из нескольких подключений.
  execute(database_,
          "CREATE TABLE IF NOT EXISTS accounts ("
          "id INTEGER PRIMARY KEY,"
          "login TEXT NOT NULL UNIQUE COLLATE BINARY,"
          "role TEXT NOT NULL CHECK(role IN ('client', 'editor')),"
          "trust REAL NOT NULL DEFAULT 0 CHECK(trust >= 0 AND trust <= 1),"
          "active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0, 1)),"
          "password_salt TEXT, password_hash TEXT)");
  execute(database_,
          "CREATE TABLE IF NOT EXISTS stories ("
          "id INTEGER PRIMARY KEY, category TEXT NOT NULL,"
          "topic TEXT NOT NULL,"
          "published_at INTEGER, UNIQUE(category, topic))");
  execute(database_,
          "CREATE TABLE IF NOT EXISTS notes ("
          "id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL REFERENCES "
          "accounts(id), request_id TEXT NOT NULL, story_id INTEGER "
          "REFERENCES stories(id), category TEXT NOT NULL, topic TEXT NOT "
          "NULL, event_at INTEGER NOT NULL, received_at INTEGER NOT NULL,"
          "status TEXT NOT NULL CHECK(status IN ('accepted', 'rejected')),"
          "reason TEXT NOT NULL, trust_at_receipt REAL NOT NULL DEFAULT 0,"
          "ready_at_receipt INTEGER NOT NULL DEFAULT 0,"
          "UNIQUE(account_id, request_id))");
  execute(database_,
          "CREATE TABLE IF NOT EXISTS votes ("
          "id INTEGER PRIMARY KEY, story_id INTEGER NOT NULL REFERENCES "
          "stories(id), account_id INTEGER NOT NULL REFERENCES accounts(id),"
          "trust REAL NOT NULL, evidence_at INTEGER NOT NULL,"
          "received_at INTEGER NOT NULL, received_order INTEGER NOT NULL "
          "DEFAULT 0, frozen_rank INTEGER,"
          "UNIQUE(story_id, account_id))");
  execute(database_,
          "CREATE TABLE IF NOT EXISTS notifications ("
          "id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL REFERENCES "
          "accounts(id), message TEXT NOT NULL, created_at INTEGER NOT NULL,"
          "acknowledged_at INTEGER)");
  execute(database_,
          "CREATE TABLE IF NOT EXISTS settings ("
          "name TEXT PRIMARY KEY, value REAL NOT NULL)");
  const Settings defaults;
  Statement insert_defaults(database_,
                            "INSERT OR IGNORE INTO settings(name, value) VALUES"
                            "(?, ?), (?, ?), (?, ?)");
  insert_defaults.bind(kFirstSqlParameter, "threshold");
  insert_defaults.bind(kSecondSqlParameter, defaults.threshold);
  insert_defaults.bind(kThirdSqlParameter, "half_life_seconds");
  insert_defaults.bind(kFourthSqlParameter, defaults.half_life_seconds);
  insert_defaults.bind(kFifthSqlParameter, "congratulations_count");
  insert_defaults.bind(kSixthSqlParameter, defaults.congratulations_count);
  insert_defaults.step();

  // Старые базы дополняются полем для восстановления порядка источников.
  bool has_received_order = false;
  Statement columns(database_, "PRAGMA table_info(votes)");
  while (columns.step() == SQLITE_ROW) {
    if (columns.text(kSecondColumn) == "received_order") {
      has_received_order = true;
    }
  }
  if (!has_received_order) {
    execute(database_,
            "ALTER TABLE votes ADD COLUMN received_order INTEGER NOT "
            "NULL DEFAULT 0");
  }
}

// Добавляет предусмотренных заданием клиентов, не меняя существующие записи.
void Store::seed_initial_clients() {
  std::lock_guard lock(mutex_);
  const std::array<std::pair<const char*, double>, kSeedClientCount> clients{{
      {"Сахарисса Крипслок", kSeedTrustForSugarissa},
      {"Избирательно Честный Тонни", kSeedTrustForSelectiveTony},
      {"Иногда Слепой Пью", kSeedTrustForSometimesPugh},
  }};
  for (const auto& [login, trust] : clients) {
    Statement statement(database_,
                        "INSERT OR IGNORE INTO accounts(login, role, trust) "
                        "VALUES(?, 'client', ?)");
    statement.bind(kFirstSqlParameter, std::string(login));
    statement.bind(kSecondSqlParameter, trust);
    statement.step();
  }
}

// Проверяет наличие хотя бы одной активной учётной записи редактора.
bool Store::has_active_editor() {
  std::lock_guard lock(mutex_);
  Statement statement(database_,
                      "SELECT 1 FROM accounts WHERE role='editor' AND "
                      "active=1 LIMIT 1");
  return statement.step() == SQLITE_ROW;
}

// Проверяет активность аккаунта account_id перед обработкой команды.
bool Store::account_is_active(std::int64_t account_id) {
  std::lock_guard lock(mutex_);
  Statement statement(database_,
                      "SELECT 1 FROM accounts WHERE id=? AND active=1");
  statement.bind(kFirstSqlParameter, account_id);
  return statement.step() == SQLITE_ROW;
}

// Возвращает максимальный сохранённый ранг для продолжения нумерации после
// перезапуска сервера.
std::int64_t Store::last_received_order() {
  std::lock_guard lock(mutex_);
  Statement statement(database_,
                      "SELECT COALESCE(MAX(received_order), 0) "
                      "FROM votes");
  statement.step();
  return statement.integer(kFirstColumn);
}

// login — имя аккаунта, password — проверяемый пароль. При успехе возвращает
// роль и доверие; сравнивает производный хеш, не сохраняя пароль.
Account Store::authenticate(const std::string& login,
                            const std::string& password) {
  std::lock_guard lock(mutex_);
  Statement statement(
      database_,
      "SELECT id, login, role, trust, password_salt, password_hash "
      "FROM accounts WHERE login=? AND active=1");
  statement.bind(kFirstSqlParameter, login);
  if (statement.step() != SQLITE_ROW || statement.is_null(kFifthColumn) ||
      statement.is_null(kSixthColumn)) {
    throw std::invalid_argument("invalid credentials");
  }
  const Account account = read_account(statement);
  if (!verify_password(password, statement.text(kFifthColumn),
                       statement.text(kSixthColumn))) {
    throw std::invalid_argument("invalid credentials");
  }
  return account;
}

// Создаёт учётную запись; role и login задают её тип и имя, trust — доверие
// клиента (для редактора принудительно равно нулю).
Account Store::add_account(const std::string& role, const std::string& login,
                           double trust) {
  std::lock_guard lock(mutex_);
  validate_login(login);
  if (role != "client" && role != "editor") {
    throw std::invalid_argument("role must be client or editor");
  }
  if (!std::isfinite(trust) || trust < 0.0 || trust > 1.0) {
    throw std::invalid_argument("trust must be between 0 and 1");
  }
  if (role == "editor") {
    trust = 0.0;
  }

  Statement statement(
      database_, "INSERT INTO accounts(login, role, trust) VALUES(?, ?, ?)");
  statement.bind(kFirstSqlParameter, login);
  statement.bind(kSecondSqlParameter, role);
  statement.bind(kThirdSqlParameter, trust);
  statement.step();
  return {sqlite3_last_insert_rowid(database_), login, role, trust};
}

// Меняет old_login на new_login; уникальность имени контролирует база данных.
void Store::rename_account(const std::string& old_login,
                           const std::string& new_login) {
  std::lock_guard lock(mutex_);
  validate_login(new_login);
  Statement statement(database_, "UPDATE accounts SET login=? WHERE login=?");
  statement.bind(kFirstSqlParameter, new_login);
  statement.bind(kSecondSqlParameter, old_login);
  statement.step();
  if (sqlite3_changes(database_) == 0) {
    throw std::invalid_argument("account not found");
  }
}

// Назначает клиенту login новое доверие trust; допустим диапазон от 0 до 1.
void Store::set_account_trust(const std::string& login, double trust) {
  std::lock_guard lock(mutex_);
  if (!std::isfinite(trust) || trust < 0.0 || trust > 1.0) {
    throw std::invalid_argument("trust must be between 0 and 1");
  }
  Statement statement(database_,
                      "UPDATE accounts SET trust=? WHERE login=? AND "
                      "role='client'");
  statement.bind(kFirstSqlParameter, trust);
  statement.bind(kSecondSqlParameter, login);
  statement.step();
  if (sqlite3_changes(database_) == 0) {
    throw std::invalid_argument("client account not found");
  }
}

// login — имя аккаунта, password — новый пароль. Сохраняет соль и хеш вместо
// исходного значения.
void Store::set_account_password(const std::string& login,
                                 const std::string& password) {
  std::lock_guard lock(mutex_);
  validate_password(password);
  const PasswordDigest digest = make_password_digest(password);
  Statement statement(database_,
                      "UPDATE accounts SET password_salt=?, "
                      "password_hash=? WHERE login=?");
  statement.bind(kFirstSqlParameter, digest.salt);
  statement.bind(kSecondSqlParameter, digest.hash);
  statement.bind(kThirdSqlParameter, login);
  statement.step();
  if (sqlite3_changes(database_) == 0) {
    throw std::invalid_argument("account not found");
  }
}

// account_id — ID вошедшего пользователя, password — новый пароль. Сохраняет
// производные данные, не исходное значение.
void Store::set_own_password(std::int64_t account_id,
                             const std::string& password) {
  std::lock_guard lock(mutex_);
  validate_password(password);
  const PasswordDigest digest = make_password_digest(password);
  Statement statement(database_,
                      "UPDATE accounts SET password_salt=?, "
                      "password_hash=? WHERE id=? AND active=1");
  statement.bind(kFirstSqlParameter, digest.salt);
  statement.bind(kSecondSqlParameter, digest.hash);
  statement.bind(kThirdSqlParameter, account_id);
  statement.step();
  if (sqlite3_changes(database_) == 0) {
    throw std::invalid_argument("active account not found");
  }
}

// Отключает login вместо удаления истории; current_account_id нельзя отключить
// этой командой, а последнего редактора нельзя оставить без доступа.
void Store::delete_account(const std::string& login,
                           std::int64_t current_account_id) {
  std::lock_guard lock(mutex_);
  Statement find(database_,
                 "SELECT id, role, active FROM accounts WHERE login=?");
  find.bind(kFirstSqlParameter, login);
  if (find.step() != SQLITE_ROW) {
    throw std::invalid_argument("account not found");
  }
  const std::int64_t account_id = find.integer(kFirstColumn);
  const std::string role = find.text(kSecondColumn);
  const bool active = find.integer(kThirdColumn) != 0;
  if (account_id == current_account_id) {
    throw std::invalid_argument("cannot delete the current account");
  }
  if (role == "editor" && active) {
    Statement editors(database_,
                      "SELECT COUNT(*) FROM accounts WHERE role='editor' "
                      "AND active=1");
    editors.step();
    if (editors.integer(kFirstColumn) <= 1) {
      throw std::invalid_argument("cannot disable the last editor");
    }
  }
  Statement update(database_, "UPDATE accounts SET active=0 WHERE id=?");
  update.bind(kFirstSqlParameter, account_id);
  update.step();
}

// Повторно активирует аккаунт login, сохраняя его историю и настройки.
void Store::enable_account(const std::string& login) {
  std::lock_guard lock(mutex_);
  Statement statement(database_, "UPDATE accounts SET active=1 WHERE login=?");
  statement.bind(kFirstSqlParameter, login);
  statement.step();
  if (sqlite3_changes(database_) == 0) {
    throw std::invalid_argument("account not found");
  }
}

// Возвращает редакторский список аккаунтов, не выдавая сохранённые хеши.
Rows Store::list_accounts() {
  std::lock_guard lock(mutex_);
  Statement statement(database_,
                      "SELECT login, role, trust, active, "
                      "password_hash IS NOT NULL FROM accounts "
                      "ORDER BY role, login");
  Rows rows;
  while (statement.step() == SQLITE_ROW) {
    rows.push_back(
        {statement.text(kFirstColumn), statement.text(kSecondColumn),
         format_number(statement.number(kThirdColumn)),
         statement.integer(kFourthColumn) != 0 ? "active" : "disabled",
         statement.integer(kFifthColumn) != 0 ? "set" : "not-set"});
  }
  return rows;
}

// Читает параметры редакции под блокировкой соединения с базой.
Settings Store::settings() {
  std::lock_guard lock(mutex_);
  return read_settings(database_);
}

// Проверяет допустимый диапазон параметра name и сохраняет его значение value.
void Store::set_setting(const std::string& name, double value) {
  std::lock_guard lock(mutex_);
  if (!std::isfinite(value)) {
    throw std::invalid_argument("setting must be a finite number");
  }
  if (name == "threshold") {
    if (value <= 0.0 || value > settings_limits::kMaximumThreshold) {
      throw std::invalid_argument("threshold must be above 0");
    }
  } else if (name == "half_life_seconds") {
    if (value < settings_limits::kMinimumHalfLifeSeconds ||
        value > settings_limits::kMaximumHalfLifeSeconds) {
      throw std::invalid_argument(
          "half-life must be between " +
          std::to_string(settings_limits::kMinimumHalfLifeSeconds) + " and " +
          std::to_string(settings_limits::kMaximumHalfLifeSeconds) +
          " seconds");
    }
    value = std::round(value);
  } else if (name == "congratulations_count") {
    if (value < 0.0 || value > settings_limits::kMaximumCongratulationsCount ||
        std::floor(value) != value) {
      throw std::invalid_argument(
          "congratulations count must be an integer from 0 to " +
          std::to_string(settings_limits::kMaximumCongratulationsCount));
    }
  } else {
    throw std::invalid_argument("unknown setting");
  }

  Statement statement(database_,
                      "INSERT INTO settings(name, value) VALUES(?, ?) "
                      "ON CONFLICT(name) DO UPDATE SET value=excluded.value");
  statement.bind(kFirstSqlParameter, name);
  statement.bind(kSecondSqlParameter, value);
  statement.step();
}

}  // namespace edition
