#include <sqlite3.h>

#include <stdexcept>
#include <vector>

#include "store_internal.hpp"

namespace edition {
using namespace store_detail;
namespace {
constexpr int kPercentagePrecision = 1;
constexpr int kAverageRankPrecision = 2;
constexpr int kTrustPercentageMultiplier = 100;
}  // namespace

// Собирает статистику клиента login; вызывающие методы уже удерживают mutex_.
Rows Store::statistics_for_login(const std::string& login) {
  Statement account(database_,
                    "SELECT id FROM accounts WHERE login=? "
                    "AND role='client'");
  account.bind(kFirstSqlParameter, login);
  if (account.step() != SQLITE_ROW) {
    throw std::invalid_argument("client account not found");
  }
  const std::int64_t account_id = account.integer(kFirstColumn);
  Statement totals(
      database_,
      "SELECT (SELECT COUNT(*) FROM notes n WHERE n.account_id=? AND "
      "n.status='accepted'),"
      "(SELECT COUNT(DISTINCT n.story_id) FROM notes n WHERE "
      "n.account_id=? AND n.status='accepted'),"
      "(SELECT COUNT(*) FROM votes v JOIN stories s ON s.id=v.story_id "
      "WHERE v.account_id=? AND s.published_at IS NOT NULL),"
      "(SELECT COUNT(*) FROM votes v JOIN stories s ON s.id=v.story_id "
      "WHERE v.account_id=? AND s.published_at IS NOT NULL AND "
      "v.frozen_rank=1),"
      "(SELECT AVG(v.frozen_rank) FROM votes v JOIN stories s ON "
      "s.id=v.story_id WHERE v.account_id=? AND "
      "s.published_at IS NOT NULL)");
  totals.bind(kFirstSqlParameter, account_id);
  totals.bind(kSecondSqlParameter, account_id);
  totals.bind(kThirdSqlParameter, account_id);
  totals.bind(kFourthSqlParameter, account_id);
  totals.bind(kFifthSqlParameter, account_id);
  totals.step();
  const std::int64_t accepted = totals.integer(kFirstColumn);
  const std::int64_t touched = totals.integer(kSecondColumn);
  const std::int64_t published = totals.integer(kThirdColumn);
  const std::int64_t scoops = totals.integer(kFourthColumn);
  const double average_rank =
      totals.is_null(kFifthColumn) ? 0.0 : totals.number(kFifthColumn);
  // Доля считается среди затронутых сюжетов; средний ранг — только среди
  // выпущенных.
  const double share = touched == 0 ? 0.0
                                    : static_cast<double>(published) /
                                          static_cast<double>(touched);
  return {
      {"stats", login, std::to_string(accepted), std::to_string(published),
       format_number(share * kTrustPercentageMultiplier, kPercentagePrecision),
       std::to_string(touched), std::to_string(scoops),
       average_rank == 0.0
           ? "-"
           : format_number(average_rank, kAverageRankPrecision)}};
}

// Возвращает личную статистику активного клиента account_id.
Rows Store::statistics(std::int64_t account_id) {
  std::lock_guard lock(mutex_);
  Statement account(database_,
                    "SELECT login, role, active FROM accounts WHERE id=?");
  account.bind(kFirstSqlParameter, account_id);
  if (account.step() != SQLITE_ROW || account.text(kSecondColumn) != "client" ||
      account.integer(kThirdColumn) == 0) {
    throw std::invalid_argument("active client account required");
  }
  return statistics_for_login(account.text(kFirstColumn));
}

// Возвращает статистику клиента login для редакторского просмотра.
Rows Store::statistics_for_user(const std::string& login) {
  std::lock_guard lock(mutex_);
  return statistics_for_login(login);
}

// Собирает статистику всех клиентов; порядок строк определяется логинами.
Rows Store::all_statistics() {
  std::lock_guard lock(mutex_);
  Statement accounts(database_,
                     "SELECT login FROM accounts WHERE role='client' "
                     "ORDER BY login");
  std::vector<std::string> logins;
  while (accounts.step() == SQLITE_ROW) {
    logins.push_back(accounts.text(kFirstColumn));
  }
  Rows rows;
  for (const std::string& login : logins) {
    Rows account_rows = statistics_for_login(login);
    rows.insert(rows.end(), account_rows.begin(), account_rows.end());
  }
  return rows;
}

// Возвращает ещё не подтверждённые сообщения account_id для повторной доставки.
Rows Store::pending_notifications(std::int64_t account_id) {
  std::lock_guard lock(mutex_);
  // Неподтверждённые сообщения остаются доступны при переподключении.
  Statement statement(database_,
                      "SELECT id, created_at, message FROM notifications WHERE "
                      "account_id=? AND acknowledged_at IS NULL ORDER BY id");
  statement.bind(kFirstSqlParameter, account_id);
  Rows rows;
  while (statement.step() == SQLITE_ROW) {
    rows.push_back({std::to_string(statement.integer(kFirstColumn)),
                    format_utc_time(statement.integer(kSecondColumn)),
                    statement.text(kThirdColumn)});
  }
  return rows;
}

// Подтверждает уведомление notification_id только его владельцу account_id;
// повторное подтверждение уже обработанного сообщения идемпотентно.
void Store::acknowledge_notification(std::int64_t account_id,
                                     std::int64_t notification_id) {
  std::lock_guard lock(mutex_);
  Statement statement(
      database_,
      "UPDATE notifications SET acknowledged_at=? WHERE id=? AND "
      "account_id=? AND acknowledged_at IS NULL");
  statement.bind(kFirstSqlParameter, current_time());
  statement.bind(kSecondSqlParameter, notification_id);
  statement.bind(kThirdSqlParameter, account_id);
  statement.step();
  if (sqlite3_changes(database_) == 0) {
    Statement existing(database_,
                       "SELECT 1 FROM notifications WHERE id=? AND "
                       "account_id=? AND acknowledged_at IS NOT NULL");
    existing.bind(kFirstSqlParameter, notification_id);
    existing.bind(kSecondSqlParameter, account_id);
    if (existing.step() != SQLITE_ROW) {
      throw std::invalid_argument("notification not found");
    }
  }
}

}  // namespace edition
