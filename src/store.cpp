#include "store.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace edition {
namespace {

constexpr int kPasswordIterations = 240000;
constexpr int kPasswordSaltBytes = 16;
constexpr int kPasswordHashBytes = 32;

std::runtime_error database_error(sqlite3* database,
                                  const std::string& action) {
    return std::runtime_error(action + ": " + sqlite3_errmsg(database));
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) !=
            SQLITE_OK) {
            throw database_error(database_, "cannot prepare SQL statement");
        }
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(),
                              static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw database_error(database_, "cannot bind text value");
        }
    }

    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw database_error(database_, "cannot bind integer value");
        }
    }

    void bind(int index, double value) {
        if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK) {
            throw database_error(database_, "cannot bind numeric value");
        }
    }

    void bind_null(int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw database_error(database_, "cannot bind null value");
        }
    }

    int step() {
        const int result = sqlite3_step(statement_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) {
            throw database_error(database_, "SQL statement failed");
        }
        return result;
    }

    std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        return value == nullptr
                   ? std::string{}
                   : reinterpret_cast<const char*>(value);
    }

    std::int64_t integer(int column) const {
        return sqlite3_column_int64(statement_, column);
    }

    double number(int column) const {
        return sqlite3_column_double(statement_, column);
    }

    bool is_null(int column) const {
        return sqlite3_column_type(statement_, column) == SQLITE_NULL;
    }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* database, const char* sql) {
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string message = error == nullptr ? sqlite3_errmsg(database)
                                                    : error;
        sqlite3_free(error);
        throw std::runtime_error("SQL operation failed: " + message);
    }
}

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_(database) {
        execute(database_, "BEGIN IMMEDIATE");
    }

    ~Transaction() {
        if (active_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void commit() {
        execute(database_, "COMMIT");
        active_ = false;
    }

private:
    sqlite3* database_ = nullptr;
    bool active_ = true;
};

std::string hex_encode(const unsigned char* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        result[i * 2] = digits[data[i] >> 4];
        result[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return result;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::vector<unsigned char> hex_decode(const std::string& value) {
    if (value.size() % 2 != 0) {
        return {};
    }
    std::vector<unsigned char> result(value.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i) {
        const int high = hex_value(value[i * 2]);
        const int low = hex_value(value[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        result[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return result;
}

struct PasswordDigest {
    std::string salt;
    std::string hash;
};

PasswordDigest make_password_digest(const std::string& password) {
    std::array<unsigned char, kPasswordSaltBytes> salt{};
    std::array<unsigned char, kPasswordHashBytes> hash{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          kPasswordIterations, EVP_sha256(),
                          static_cast<int>(hash.size()), hash.data()) != 1) {
        throw std::runtime_error("could not derive password hash");
    }
    return {hex_encode(salt.data(), salt.size()),
            hex_encode(hash.data(), hash.size())};
}

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
            static_cast<int>(actual.size()), actual.data()) != 1) {
        throw std::runtime_error("could not verify password");
    }
    return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

std::string format_number(double value, int precision = 3) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

Settings read_settings(sqlite3* database) {
    Settings result;
    Statement statement(database, "SELECT name, value FROM settings");
    while (statement.step() == SQLITE_ROW) {
        const std::string name = statement.text(0);
        if (name == "threshold") {
            result.threshold = statement.number(1);
        } else if (name == "half_life_seconds") {
            result.half_life_seconds =
                static_cast<std::int64_t>(statement.number(1));
        } else if (name == "congratulations_count") {
            result.congratulations_count =
                static_cast<std::int64_t>(statement.number(1));
        }
    }
    return result;
}

Account read_account(Statement& statement) {
    return {statement.integer(0), statement.text(1), statement.text(2),
            statement.number(3)};
}

void validate_password(const std::string& password) {
    if (password.size() < 8 || !is_safe_field(password, 1024)) {
        throw std::invalid_argument(
            "password must have 8 to 1024 printable characters");
    }
}

void validate_login(const std::string& login) {
    if (!is_safe_field(login, 128)) {
        throw std::invalid_argument("login must have 1 to 128 characters");
    }
}

}  // namespace

Store::Store(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
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
    sqlite3_busy_timeout(database_, 5000);
    execute(database_, "PRAGMA foreign_keys = ON");
    execute(database_, "PRAGMA journal_mode = WAL");
    initialize_schema();
}

Store::~Store() {
    if (database_ != nullptr) {
        sqlite3_close(database_);
    }
}

void Store::initialize_schema() {
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
    execute(database_,
            "INSERT OR IGNORE INTO settings(name, value) VALUES"
            "('threshold', 1.01), ('half_life_seconds', 21600),"
            "('congratulations_count', 3)");

    bool has_received_order = false;
    Statement columns(database_, "PRAGMA table_info(votes)");
    while (columns.step() == SQLITE_ROW) {
        if (columns.text(1) == "received_order") {
            has_received_order = true;
        }
    }
    if (!has_received_order) {
        execute(database_,
                "ALTER TABLE votes ADD COLUMN received_order INTEGER NOT "
                "NULL DEFAULT 0");
    }
}

void Store::seed_initial_clients() {
    std::lock_guard lock(mutex_);
    const std::array<std::pair<const char*, double>, 3> clients{{
        {"Сахарисса Крипслок", 1.00},
        {"Избирательно Честный Тонни", 0.25},
        {"Иногда Слепой Пью", 0.35},
    }};
    for (const auto& [login, trust] : clients) {
        Statement statement(
            database_,
            "INSERT OR IGNORE INTO accounts(login, role, trust) "
            "VALUES(?1, 'client', ?2)");
        statement.bind(1, std::string(login));
        statement.bind(2, trust);
        statement.step();
    }
}

bool Store::has_active_editor() {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
                        "SELECT 1 FROM accounts WHERE role='editor' AND "
                        "active=1 LIMIT 1");
    return statement.step() == SQLITE_ROW;
}

bool Store::account_is_active(std::int64_t account_id) {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
                        "SELECT 1 FROM accounts WHERE id=?1 AND active=1");
    statement.bind(1, account_id);
    return statement.step() == SQLITE_ROW;
}

std::int64_t Store::last_received_order() {
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT COALESCE(MAX(received_order), 0) "
                                   "FROM votes");
    statement.step();
    return statement.integer(0);
}

Account Store::authenticate(const std::string& login,
                            const std::string& password) {
    std::lock_guard lock(mutex_);
    Statement statement(
        database_,
        "SELECT id, login, role, trust, password_salt, password_hash "
        "FROM accounts WHERE login=?1 AND active=1");
    statement.bind(1, login);
    if (statement.step() != SQLITE_ROW || statement.is_null(4) ||
        statement.is_null(5)) {
        throw std::invalid_argument("invalid credentials");
    }
    const Account account = read_account(statement);
    if (!verify_password(password, statement.text(4), statement.text(5))) {
        throw std::invalid_argument("invalid credentials");
    }
    return account;
}

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
        database_,
        "INSERT INTO accounts(login, role, trust) VALUES(?1, ?2, ?3)");
    statement.bind(1, login);
    statement.bind(2, role);
    statement.bind(3, trust);
    statement.step();
    return {sqlite3_last_insert_rowid(database_), login, role, trust};
}

void Store::rename_account(const std::string& old_login,
                           const std::string& new_login) {
    std::lock_guard lock(mutex_);
    validate_login(new_login);
    Statement statement(database_,
                        "UPDATE accounts SET login=?1 WHERE login=?2");
    statement.bind(1, new_login);
    statement.bind(2, old_login);
    statement.step();
    if (sqlite3_changes(database_) == 0) {
        throw std::invalid_argument("account not found");
    }
}

void Store::set_account_trust(const std::string& login, double trust) {
    std::lock_guard lock(mutex_);
    if (!std::isfinite(trust) || trust < 0.0 || trust > 1.0) {
        throw std::invalid_argument("trust must be between 0 and 1");
    }
    Statement statement(database_,
                        "UPDATE accounts SET trust=?1 WHERE login=?2 AND "
                        "role='client'");
    statement.bind(1, trust);
    statement.bind(2, login);
    statement.step();
    if (sqlite3_changes(database_) == 0) {
        throw std::invalid_argument("client account not found");
    }
}

void Store::set_account_password(const std::string& login,
                                 const std::string& password) {
    std::lock_guard lock(mutex_);
    validate_password(password);
    const PasswordDigest digest = make_password_digest(password);
    Statement statement(database_,
                        "UPDATE accounts SET password_salt=?1, "
                        "password_hash=?2 WHERE login=?3");
    statement.bind(1, digest.salt);
    statement.bind(2, digest.hash);
    statement.bind(3, login);
    statement.step();
    if (sqlite3_changes(database_) == 0) {
        throw std::invalid_argument("account not found");
    }
}

void Store::set_own_password(std::int64_t account_id,
                             const std::string& password) {
    std::lock_guard lock(mutex_);
    validate_password(password);
    const PasswordDigest digest = make_password_digest(password);
    Statement statement(database_,
                        "UPDATE accounts SET password_salt=?1, "
                        "password_hash=?2 WHERE id=?3 AND active=1");
    statement.bind(1, digest.salt);
    statement.bind(2, digest.hash);
    statement.bind(3, account_id);
    statement.step();
    if (sqlite3_changes(database_) == 0) {
        throw std::invalid_argument("active account not found");
    }
}

void Store::delete_account(const std::string& login,
                           std::int64_t current_account_id) {
    std::lock_guard lock(mutex_);
    Statement find(database_,
                   "SELECT id, role, active FROM accounts WHERE login=?1");
    find.bind(1, login);
    if (find.step() != SQLITE_ROW) {
        throw std::invalid_argument("account not found");
    }
    const std::int64_t account_id = find.integer(0);
    const std::string role = find.text(1);
    const bool active = find.integer(2) != 0;
    if (account_id == current_account_id) {
        throw std::invalid_argument("cannot delete the current account");
    }
    if (role == "editor" && active) {
        Statement editors(database_,
                          "SELECT COUNT(*) FROM accounts WHERE role='editor' "
                          "AND active=1");
        editors.step();
        if (editors.integer(0) <= 1) {
            throw std::invalid_argument("cannot disable the last editor");
        }
    }
    Statement update(database_,
                     "UPDATE accounts SET active=0 WHERE id=?1");
    update.bind(1, account_id);
    update.step();
}

void Store::enable_account(const std::string& login) {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
                        "UPDATE accounts SET active=1 WHERE login=?1");
    statement.bind(1, login);
    statement.step();
    if (sqlite3_changes(database_) == 0) {
        throw std::invalid_argument("account not found");
    }
}

Rows Store::list_accounts() {
    std::lock_guard lock(mutex_);
    Statement statement(database_,
                        "SELECT login, role, trust, active, "
                        "password_hash IS NOT NULL FROM accounts "
                        "ORDER BY role, login");
    Rows rows;
    while (statement.step() == SQLITE_ROW) {
        rows.push_back({statement.text(0), statement.text(1),
                        format_number(statement.number(2)),
                        statement.integer(3) != 0 ? "active" : "disabled",
                        statement.integer(4) != 0 ? "set" : "not-set"});
    }
    return rows;
}

Settings Store::settings() {
    std::lock_guard lock(mutex_);
    return read_settings(database_);
}

void Store::set_setting(const std::string& name, double value) {
    std::lock_guard lock(mutex_);
    if (!std::isfinite(value)) {
        throw std::invalid_argument("setting must be a finite number");
    }
    if (name == "threshold") {
        if (value <= 0.0 || value > 100000.0) {
            throw std::invalid_argument("threshold must be above 0");
        }
    } else if (name == "half_life_seconds") {
        if (value < 60.0 || value > 315360000.0) {
            throw std::invalid_argument(
                "half-life must be between 60 and 315360000 seconds");
        }
        value = std::round(value);
    } else if (name == "congratulations_count") {
        if (value < 0.0 || value > 100.0 || std::floor(value) != value) {
            throw std::invalid_argument(
                "congratulations count must be an integer from 0 to 100");
        }
    } else {
        throw std::invalid_argument("unknown setting");
    }

    Statement statement(database_,
                        "INSERT INTO settings(name, value) VALUES(?1, ?2) "
                        "ON CONFLICT(name) DO UPDATE SET value=excluded.value");
    statement.bind(1, name);
    statement.bind(2, value);
    statement.step();
}

double Store::story_trust(std::int64_t story_id, std::int64_t now,
                          const Settings& current_settings) {
    // Every source contributes its stored trust, decayed from event time.
    Statement statement(database_,
                        "SELECT trust, evidence_at FROM votes "
                        "WHERE story_id=?1");
    statement.bind(1, story_id);
    double total = 0.0;
    while (statement.step() == SQLITE_ROW) {
        const double age = static_cast<double>(
            std::max<std::int64_t>(0, now - statement.integer(1)));
        const double periods =
            age / static_cast<double>(current_settings.half_life_seconds);
        total += statement.number(0) * std::pow(0.5, periods);
    }
    return total;
}

Submission Store::submit_note(std::int64_t account_id,
                              const std::string& request_id,
                              const std::string& category,
                              const std::string& topic,
                              std::int64_t event_time,
                              std::int64_t received_at,
                              std::int64_t received_order) {
    std::lock_guard lock(mutex_);
    if (!is_safe_field(request_id, 128) ||
        !is_safe_field(category, 128) || !is_safe_field(topic, 1024)) {
        throw std::invalid_argument("invalid request id, category or topic");
    }
    Transaction transaction(database_);

    Statement existing(
        database_,
        "SELECT category, topic, event_at, status, reason, story_id, "
        "trust_at_receipt, ready_at_receipt FROM notes "
        "WHERE account_id=?1 AND request_id=?2");
    existing.bind(1, account_id);
    existing.bind(2, request_id);
    if (existing.step() == SQLITE_ROW) {
        if (existing.text(0) != category || existing.text(1) != topic ||
            existing.integer(2) != event_time) {
            transaction.commit();
            return {"rejected", "request_id_conflict", 0, 0.0, false, false};
        }
        Submission result{"duplicate", "request_already_received",
                          existing.is_null(5) ? 0 : existing.integer(5),
                          existing.number(6), existing.integer(7) != 0, true};
        transaction.commit();
        return result;
    }

    Statement account(database_,
                      "SELECT trust FROM accounts WHERE id=?1 "
                      "AND role='client' "
                      "AND active=1");
    account.bind(1, account_id);
    if (account.step() != SQLITE_ROW) {
        throw std::invalid_argument("active client account required");
    }
    const double source_trust = account.number(0);

    std::int64_t story_id = 0;
    bool already_published = false;
    Statement story(database_,
                    "SELECT id, published_at FROM stories WHERE category=?1 "
                    "AND topic=?2");
    story.bind(1, category);
    story.bind(2, topic);
    if (story.step() == SQLITE_ROW) {
        story_id = story.integer(0);
        already_published = !story.is_null(1);
    } else {
        Statement insert_story(
            database_,
            "INSERT INTO stories(category, topic) VALUES(?1, ?2)");
        insert_story.bind(1, category);
        insert_story.bind(2, topic);
        insert_story.step();
        story_id = sqlite3_last_insert_rowid(database_);
    }

    if (already_published) {
        Statement insert_note(
            database_,
            "INSERT INTO notes(account_id, request_id, story_id, category, "
            "topic, event_at, received_at, status, reason) VALUES"
            "(?1, ?2, ?3, ?4, ?5, ?6, ?7, 'rejected', 'story_published')");
        insert_note.bind(1, account_id);
        insert_note.bind(2, request_id);
        insert_note.bind(3, story_id);
        insert_note.bind(4, category);
        insert_note.bind(5, topic);
        insert_note.bind(6, event_time);
        insert_note.bind(7, received_at);
        insert_note.step();
        transaction.commit();
        return {"rejected", "story_published", story_id, 0.0, false, false};
    }

    Statement insert_note(
        database_,
        "INSERT INTO notes(account_id, request_id, story_id, category, topic, "
        "event_at, received_at, status, reason) VALUES"
        "(?1, ?2, ?3, ?4, ?5, ?6, ?7, 'accepted', 'accepted')");
    insert_note.bind(1, account_id);
    insert_note.bind(2, request_id);
    insert_note.bind(3, story_id);
    insert_note.bind(4, category);
    insert_note.bind(5, topic);
    insert_note.bind(6, event_time);
    insert_note.bind(7, received_at);
    insert_note.step();

    const std::int64_t evidence_at = std::min(event_time, received_at);
    Statement insert_vote(
        database_,
        "INSERT OR IGNORE INTO votes(story_id, account_id, trust, "
        "evidence_at, received_at, received_order) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
    insert_vote.bind(1, story_id);
    insert_vote.bind(2, account_id);
    insert_vote.bind(3, source_trust);
    insert_vote.bind(4, evidence_at);
    insert_vote.bind(5, received_at);
    insert_vote.bind(6, received_order);
    insert_vote.step();
    const bool first_source = sqlite3_changes(database_) != 0;

    const Settings current_settings = read_settings(database_);
    const double score =
        story_trust(story_id, received_at, current_settings);
    const bool ready = score >= current_settings.threshold;
    const std::string reason =
        first_source ? "first_source" : "already_counted";
    Statement update_receipt(
        database_,
        "UPDATE notes SET reason=?1, trust_at_receipt=?2, "
        "ready_at_receipt=?3 WHERE account_id=?4 AND request_id=?5");
    update_receipt.bind(1, reason);
    update_receipt.bind(2, score);
    update_receipt.bind(3, static_cast<std::int64_t>(ready ? 1 : 0));
    update_receipt.bind(4, account_id);
    update_receipt.bind(5, request_id);
    update_receipt.step();
    transaction.commit();
    return {"accepted", reason, story_id, score, ready, false};
}

Rows Store::list_stories() {
    std::lock_guard lock(mutex_);
    const Settings current_settings = read_settings(database_);
    Statement statement(database_,
                        "SELECT s.id, s.category, s.topic, s.published_at, "
                        "COUNT(v.id) FROM stories s LEFT JOIN votes v ON "
                        "v.story_id=s.id GROUP BY s.id ORDER BY s.id");
    Rows rows;
    while (statement.step() == SQLITE_ROW) {
        const std::int64_t story_id = statement.integer(0);
        const bool published = !statement.is_null(3);
        const double score = published
                                 ? 0.0
                                 : story_trust(story_id, current_time(),
                                               current_settings);
        const bool ready = score >= current_settings.threshold;
        const std::string readiness =
            published ? "published" : (ready ? "ready" : "below-threshold");
        rows.push_back({std::to_string(story_id), statement.text(1),
                        statement.text(2), published ? "published" : "open",
                        published ? "-" : format_number(score),
                        format_number(current_settings.threshold), readiness,
                        std::to_string(statement.integer(4))});
    }
    return rows;
}

Rows Store::story_details(std::int64_t story_id) {
    std::lock_guard lock(mutex_);
    const Settings current_settings = read_settings(database_);
    Statement story(database_,
                    "SELECT category, topic, published_at FROM stories "
                    "WHERE id=?1");
    story.bind(1, story_id);
    if (story.step() != SQLITE_ROW) {
        throw std::invalid_argument("story not found");
    }
    const bool published = !story.is_null(2);
    const double score = published
                             ? 0.0
                             : story_trust(story_id, current_time(),
                                           current_settings);
    const std::string readiness =
        published ? "published"
                  : (score >= current_settings.threshold ? "ready"
                                                        : "below-threshold");
    Rows rows{{"story", std::to_string(story_id), story.text(0), story.text(1),
               published ? "published" : "open",
               published ? "-" : format_number(score),
               readiness,
               format_number(current_settings.threshold)}};

    Statement sources(
        database_,
        "SELECT a.login, v.trust, v.evidence_at, v.received_at, "
        "v.frozen_rank, v.id, (SELECT COUNT(*) FROM notes n WHERE "
        "n.story_id=v.story_id AND n.account_id=v.account_id AND "
        "n.status='accepted') FROM votes v JOIN accounts a ON "
        "a.id=v.account_id WHERE v.story_id=?1 "
        "ORDER BY v.received_order, v.id");
    sources.bind(1, story_id);
    std::int64_t provisional_rank = 0;
    while (sources.step() == SQLITE_ROW) {
        ++provisional_rank;
        const std::int64_t rank = sources.is_null(4)
                                      ? provisional_rank
                                      : sources.integer(4);
        rows.push_back({"source", std::to_string(rank), sources.text(0),
                        format_number(sources.number(1)),
                        format_utc_time(sources.integer(2)),
                        format_utc_time(sources.integer(3)),
                        std::to_string(sources.integer(6))});
    }
    return rows;
}

Rows Store::publish_story(std::int64_t story_id) {
    std::lock_guard lock(mutex_);
    Transaction transaction(database_);
    Statement story(database_,
                    "SELECT category, topic, published_at FROM stories "
                    "WHERE id=?1");
    story.bind(1, story_id);
    if (story.step() != SQLITE_ROW) {
        throw std::invalid_argument("story not found");
    }
    if (!story.is_null(2)) {
        throw std::invalid_argument("story has already been published");
    }
    const std::string topic = story.text(1);
    const Settings current_settings = read_settings(database_);
    const std::int64_t now = current_time();
    const double score = story_trust(story_id, now, current_settings);
    if (score < current_settings.threshold) {
        throw std::invalid_argument("story is below the print threshold");
    }

    Statement sources(database_,
                      "SELECT v.id, v.account_id, a.login FROM votes v "
                      "JOIN accounts a ON a.id=v.account_id "
                      "WHERE v.story_id=?1 ORDER BY v.received_order, v.id");
    sources.bind(1, story_id);
    struct Source {
        std::int64_t vote_id;
        std::int64_t account_id;
        std::string login;
    };
    std::vector<Source> ordered_sources;
    while (sources.step() == SQLITE_ROW) {
        ordered_sources.push_back(
            {sources.integer(0), sources.integer(1), sources.text(2)});
    }

    // Freeze ranks with the queued congratulations in this transaction.
    for (std::size_t index = 0; index < ordered_sources.size(); ++index) {
        Statement rank(database_,
                       "UPDATE votes SET frozen_rank=?1 WHERE id=?2");
        rank.bind(1, static_cast<std::int64_t>(index + 1));
        rank.bind(2, ordered_sources[index].vote_id);
        rank.step();
    }
    Statement mark_published(
        database_,
        "UPDATE stories SET published_at=?1 WHERE id=?2 AND "
        "published_at IS NULL");
    mark_published.bind(1, now);
    mark_published.bind(2, story_id);
    mark_published.step();

    const std::size_t congratulations = std::min(
        ordered_sources.size(),
        static_cast<std::size_t>(current_settings.congratulations_count));
    for (std::size_t index = 0; index < congratulations; ++index) {
        std::ostringstream message;
        message << "Поздравляем, ваша заметка попала в печать. Вы были "
                << (index + 1) << "-м, кто сказал нам об этой новости.";
        Statement notification(
            database_,
            "INSERT INTO notifications(account_id, message, created_at) "
            "VALUES(?1, ?2, ?3)");
        notification.bind(1, ordered_sources[index].account_id);
        notification.bind(2, message.str());
        notification.bind(3, now);
        notification.step();
    }
    transaction.commit();
    return {{"published", std::to_string(story_id),
             std::to_string(ordered_sources.size()), format_number(score)}};
}

Rows Store::statistics_for_login(const std::string& login) {
    Statement account(database_,
                      "SELECT id FROM accounts WHERE login=?1 "
                      "AND role='client'");
    account.bind(1, login);
    if (account.step() != SQLITE_ROW) {
        throw std::invalid_argument("client account not found");
    }
    const std::int64_t account_id = account.integer(0);
    Statement totals(
        database_,
        "SELECT (SELECT COUNT(*) FROM notes n WHERE n.account_id=?1 AND "
        "n.status='accepted'),"
        "(SELECT COUNT(DISTINCT n.story_id) FROM notes n WHERE "
        "n.account_id=?1 AND n.status='accepted'),"
        "(SELECT COUNT(*) FROM votes v JOIN stories s ON s.id=v.story_id "
        "WHERE v.account_id=?1 AND s.published_at IS NOT NULL),"
        "(SELECT COUNT(*) FROM votes v JOIN stories s ON s.id=v.story_id "
        "WHERE v.account_id=?1 AND s.published_at IS NOT NULL AND "
        "v.frozen_rank=1),"
        "(SELECT AVG(v.frozen_rank) FROM votes v JOIN stories s ON "
        "s.id=v.story_id WHERE v.account_id=?1 AND "
        "s.published_at IS NOT NULL)");
    totals.bind(1, account_id);
    totals.step();
    const std::int64_t accepted = totals.integer(0);
    const std::int64_t touched = totals.integer(1);
    const std::int64_t published = totals.integer(2);
    const std::int64_t scoops = totals.integer(3);
    const double average_rank = totals.is_null(4) ? 0.0 : totals.number(4);
    const double share = touched == 0
                             ? 0.0
                             : static_cast<double>(published) /
                                   static_cast<double>(touched);
    return {{"stats", login, std::to_string(accepted),
             std::to_string(published), format_number(share * 100.0, 1),
             std::to_string(touched), std::to_string(scoops),
             average_rank == 0.0 ? "-" : format_number(average_rank, 2)}};
}

Rows Store::statistics(std::int64_t account_id) {
    std::lock_guard lock(mutex_);
    Statement account(database_,
                     "SELECT login, role, active FROM accounts WHERE id=?1");
    account.bind(1, account_id);
    if (account.step() != SQLITE_ROW || account.text(1) != "client" ||
        account.integer(2) == 0) {
        throw std::invalid_argument("active client account required");
    }
    return statistics_for_login(account.text(0));
}

Rows Store::statistics_for_user(const std::string& login) {
    std::lock_guard lock(mutex_);
    return statistics_for_login(login);
}

Rows Store::all_statistics() {
    std::lock_guard lock(mutex_);
    Statement accounts(database_,
                       "SELECT login FROM accounts WHERE role='client' "
                       "ORDER BY login");
    std::vector<std::string> logins;
    while (accounts.step() == SQLITE_ROW) {
        logins.push_back(accounts.text(0));
    }
    Rows rows;
    for (const std::string& login : logins) {
        Rows account_rows = statistics_for_login(login);
        rows.insert(rows.end(), account_rows.begin(), account_rows.end());
    }
    return rows;
}

Rows Store::pending_notifications(std::int64_t account_id) {
    std::lock_guard lock(mutex_);
    Statement statement(
        database_,
        "SELECT id, created_at, message FROM notifications WHERE "
        "account_id=?1 AND acknowledged_at IS NULL ORDER BY id");
    statement.bind(1, account_id);
    Rows rows;
    while (statement.step() == SQLITE_ROW) {
        rows.push_back({std::to_string(statement.integer(0)),
                        format_utc_time(statement.integer(1)),
                        statement.text(2)});
    }
    return rows;
}

void Store::acknowledge_notification(std::int64_t account_id,
                                    std::int64_t notification_id) {
    std::lock_guard lock(mutex_);
    Statement statement(
        database_,
        "UPDATE notifications SET acknowledged_at=?1 WHERE id=?2 AND "
        "account_id=?3 AND acknowledged_at IS NULL");
    statement.bind(1, current_time());
    statement.bind(2, notification_id);
    statement.bind(3, account_id);
    statement.step();
    if (sqlite3_changes(database_) == 0) {
        Statement existing(database_,
                           "SELECT 1 FROM notifications WHERE id=?1 AND "
                           "account_id=?2 AND acknowledged_at IS NOT NULL");
        existing.bind(1, notification_id);
        existing.bind(2, account_id);
        if (existing.step() != SQLITE_ROW) {
            throw std::invalid_argument("notification not found");
        }
    }
}

}  // namespace edition
