#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "store_internal.hpp"

namespace edition {
using namespace store_detail;
namespace {
constexpr double kHalfLifeRemainingWeight = 0.5;
}  // namespace

// Суммирует голоса сюжета story_id, затухшие к моменту now по half-life из
// current_settings; вызывается внутри уже защищённой операции хранилища.
double Store::story_trust(std::int64_t story_id, std::int64_t now,
                          const Settings& current_settings) {
  // Вклад каждого источника затухает по времени события,
  // а не по времени приёма.
  Statement statement(database_,
                      "SELECT trust, evidence_at FROM votes "
                      "WHERE story_id=?");
  statement.bind(kFirstSqlParameter, story_id);
  double total = 0.0;
  while (statement.step() == SQLITE_ROW) {
    const double age = static_cast<double>(
        std::max<std::int64_t>(0, now - statement.integer(kSecondColumn)));
    const double periods =
        age / static_cast<double>(current_settings.half_life_seconds);
    total += statement.number(kFirstColumn) *
             std::pow(kHalfLifeRemainingWeight, periods);
  }
  return total;
}

// account_id — источник; request_id — ключ повтора; category и topic — сюжет;
// event_time и received_at — время события и приёма в Unix-секундах;
// received_order — глобальный порядок получения строки.
Submission Store::submit_note(std::int64_t account_id,
                              const std::string& request_id,
                              const std::string& category,
                              const std::string& topic, std::int64_t event_time,
                              std::int64_t received_at,
                              std::int64_t received_order) {
  std::lock_guard lock(mutex_);
  if (!is_safe_field(request_id, limits::kMaximumRequestIdLength) ||
      !is_safe_field(category, limits::kMaximumCategoryLength) ||
      !is_safe_field(topic, limits::kMaximumTopicLength)) {
    throw std::invalid_argument("invalid request id, category or topic");
  }
  // Квитанция, сюжет и голос должны фиксироваться вместе; повтор запроса
  // проверяется в той же транзакции до создания нового вклада.
  Transaction transaction(database_);

  Statement existing(
      database_,
      "SELECT category, topic, event_at, status, reason, story_id, "
      "trust_at_receipt, ready_at_receipt FROM notes "
      "WHERE account_id=? AND request_id=?");
  existing.bind(kFirstSqlParameter, account_id);
  existing.bind(kSecondSqlParameter, request_id);
  if (existing.step() == SQLITE_ROW) {
    if (existing.text(kFirstColumn) != category ||
        existing.text(kSecondColumn) != topic ||
        existing.integer(kThirdColumn) != event_time) {
      transaction.commit();
      return {"rejected", "request_id_conflict", 0, 0.0, false, false};
    }
    Submission result{
        "duplicate",
        "request_already_received",
        existing.is_null(kSixthColumn) ? 0 : existing.integer(kSixthColumn),
        existing.number(kSeventhColumn),
        existing.integer(kEighthColumn) != 0,
        true};
    transaction.commit();
    return result;
  }

  Statement account(database_,
                    "SELECT trust FROM accounts WHERE id=? "
                    "AND role='client' "
                    "AND active=1");
  account.bind(kFirstSqlParameter, account_id);
  if (account.step() != SQLITE_ROW) {
    throw std::invalid_argument("active client account required");
  }
  // Доверие фиксируется в голосе: последующие изменения профиля
  // не пересчитывают уже собранные свидетельства.
  const double source_trust = account.number(kFirstColumn);

  std::int64_t story_id = 0;
  bool already_published = false;
  Statement story(database_,
                  "SELECT id, published_at FROM stories WHERE category=? "
                  "AND topic=?");
  story.bind(kFirstSqlParameter, category);
  story.bind(kSecondSqlParameter, topic);
  if (story.step() == SQLITE_ROW) {
    story_id = story.integer(kFirstColumn);
    already_published = !story.is_null(kSecondColumn);
  } else {
    Statement insert_story(database_,
                           "INSERT INTO stories(category, topic) VALUES(?, ?)");
    insert_story.bind(kFirstSqlParameter, category);
    insert_story.bind(kSecondSqlParameter, topic);
    insert_story.step();
    story_id = sqlite3_last_insert_rowid(database_);
  }

  Statement insert_note(
      database_,
      "INSERT INTO notes(account_id, request_id, story_id, category, topic, "
      "event_at, received_at, status, reason) VALUES"
      "(?, ?, ?, ?, ?, ?, ?, ?, ?)");
  insert_note.bind(kFirstSqlParameter, account_id);
  insert_note.bind(kSecondSqlParameter, request_id);
  insert_note.bind(kThirdSqlParameter, story_id);
  insert_note.bind(kFourthSqlParameter, category);
  insert_note.bind(kFifthSqlParameter, topic);
  insert_note.bind(kSixthSqlParameter, event_time);
  insert_note.bind(kSeventhSqlParameter, received_at);
  insert_note.bind(kEighthSqlParameter,
                   already_published ? "rejected" : "accepted");
  insert_note.bind(kNinthSqlParameter,
                   already_published ? "story_published" : "accepted");
  insert_note.step();

  if (already_published) {
    transaction.commit();
    return {"rejected", "story_published", story_id, 0.0, false, false};
  }

  // Будущая дата события не должна откладывать затухание за время приёма.
  const std::int64_t evidence_at = std::min(event_time, received_at);
  // Заметка сохраняется всегда, но UNIQUE(story_id, account_id)
  // разрешает клиенту только один голос за сюжет.
  Statement insert_vote(
      database_,
      "INSERT OR IGNORE INTO votes(story_id, account_id, trust, "
      "evidence_at, received_at, received_order) "
      "VALUES(?, ?, ?, ?, ?, ?)");
  insert_vote.bind(kFirstSqlParameter, story_id);
  insert_vote.bind(kSecondSqlParameter, account_id);
  insert_vote.bind(kThirdSqlParameter, source_trust);
  insert_vote.bind(kFourthSqlParameter, evidence_at);
  insert_vote.bind(kFifthSqlParameter, received_at);
  insert_vote.bind(kSixthSqlParameter, received_order);
  insert_vote.step();
  const bool first_source = sqlite3_changes(database_) != 0;

  const Settings current_settings = read_settings(database_);
  const double score = story_trust(story_id, received_at, current_settings);
  // Квитанция хранит оценку и готовность именно на момент приёма.
  const bool ready = score >= current_settings.threshold;
  const std::string reason = first_source ? "first_source" : "already_counted";
  Statement update_receipt(
      database_,
      "UPDATE notes SET reason=?, trust_at_receipt=?, "
      "ready_at_receipt=? WHERE account_id=? AND request_id=?");
  update_receipt.bind(kFirstSqlParameter, reason);
  update_receipt.bind(kSecondSqlParameter, score);
  update_receipt.bind(kThirdSqlParameter,
                      static_cast<std::int64_t>(ready ? 1 : 0));
  update_receipt.bind(kFourthSqlParameter, account_id);
  update_receipt.bind(kFifthSqlParameter, request_id);
  update_receipt.step();
  transaction.commit();
  return {"accepted", reason, story_id, score, ready, false};
}

// Возвращает сюжеты с текущей оценкой доверия и состоянием порога публикации.
Rows Store::list_stories() {
  std::lock_guard lock(mutex_);
  const Settings current_settings = read_settings(database_);
  Statement statement(database_,
                      "SELECT s.id, s.category, s.topic, s.published_at, "
                      "COUNT(v.id) FROM stories s LEFT JOIN votes v ON "
                      "v.story_id=s.id GROUP BY s.id ORDER BY s.id");
  Rows rows;
  while (statement.step() == SQLITE_ROW) {
    const std::int64_t story_id = statement.integer(kFirstColumn);
    const bool published = !statement.is_null(kFourthColumn);
    const double score =
        published ? 0.0
                  : story_trust(story_id, current_time(), current_settings);
    const bool ready = score >= current_settings.threshold;
    const std::string readiness =
        published ? "published" : (ready ? "ready" : "below-threshold");
    rows.push_back({std::to_string(story_id), statement.text(kSecondColumn),
                    statement.text(kThirdColumn),
                    published ? "published" : "open",
                    published ? "-" : format_number(score),
                    format_number(current_settings.threshold), readiness,
                    std::to_string(statement.integer(kFifthColumn))});
  }
  return rows;
}

// Формирует сведения о сюжете story_id и источниках в неизменяемом порядке
// получения, включая сохранённые после публикации ранги.
Rows Store::story_details(std::int64_t story_id) {
  std::lock_guard lock(mutex_);
  const Settings current_settings = read_settings(database_);
  Statement story(database_,
                  "SELECT category, topic, published_at FROM stories "
                  "WHERE id=?");
  story.bind(kFirstSqlParameter, story_id);
  if (story.step() != SQLITE_ROW) {
    throw std::invalid_argument("story not found");
  }
  const bool published = !story.is_null(kThirdColumn);
  const double score =
      published ? 0.0 : story_trust(story_id, current_time(), current_settings);
  const std::string readiness =
      published
          ? "published"
          : (score >= current_settings.threshold ? "ready" : "below-threshold");
  Rows rows{{"story", std::to_string(story_id), story.text(kFirstColumn),
             story.text(kSecondColumn), published ? "published" : "open",
             published ? "-" : format_number(score), readiness,
             format_number(current_settings.threshold)}};

  Statement sources(database_,
                    "SELECT a.login, v.trust, v.evidence_at, v.received_at, "
                    "v.frozen_rank, v.id, (SELECT COUNT(*) FROM notes n WHERE "
                    "n.story_id=v.story_id AND n.account_id=v.account_id AND "
                    "n.status='accepted') FROM votes v JOIN accounts a ON "
                    "a.id=v.account_id WHERE v.story_id=? "
                    "ORDER BY v.received_order, v.id");
  sources.bind(kFirstSqlParameter, story_id);
  std::int64_t provisional_rank = 0;
  while (sources.step() == SQLITE_ROW) {
    ++provisional_rank;
    const std::int64_t rank = sources.is_null(kFifthColumn)
                                  ? provisional_rank
                                  : sources.integer(kFifthColumn);
    rows.push_back({"source", std::to_string(rank), sources.text(kFirstColumn),
                    format_number(sources.number(kSecondColumn)),
                    format_utc_time(sources.integer(kThirdColumn)),
                    format_utc_time(sources.integer(kFourthColumn)),
                    std::to_string(sources.integer(kSeventhColumn))});
  }
  return rows;
}

// Публикует готовый сюжет story_id, фиксируя ранги всех источников и создавая
// уведомления первым получившим голос клиентам в одной транзакции.
Rows Store::publish_story(std::int64_t story_id) {
  std::lock_guard lock(mutex_);
  // Выпуск, фиксация полного порядка и очередь поздравлений коммитятся
  // атомарно, поэтому последующие заметки не изменят опубликованные ранги.
  Transaction transaction(database_);
  Statement story(database_,
                  "SELECT category, topic, published_at FROM stories "
                  "WHERE id=?");
  story.bind(kFirstSqlParameter, story_id);
  if (story.step() != SQLITE_ROW) {
    throw std::invalid_argument("story not found");
  }
  if (!story.is_null(kThirdColumn)) {
    throw std::invalid_argument("story has already been published");
  }
  const std::string topic = story.text(kSecondColumn);
  const Settings current_settings = read_settings(database_);
  const std::int64_t now = current_time();
  const double score = story_trust(story_id, now, current_settings);
  if (score < current_settings.threshold) {
    throw std::invalid_argument("story is below the print threshold");
  }

  Statement sources(database_,
                    "SELECT v.id, v.account_id, a.login FROM votes v "
                    "JOIN accounts a ON a.id=v.account_id "
                    "WHERE v.story_id=? ORDER BY v.received_order, v.id");
  sources.bind(kFirstSqlParameter, story_id);
  // Данные голоса в принятом порядке нужны для фиксации рангов и уведомлений.
  struct Source {
    std::int64_t vote_id;
    std::int64_t account_id;
    std::string login;
  };
  std::vector<Source> ordered_sources;
  while (sources.step() == SQLITE_ROW) {
    ordered_sources.push_back({sources.integer(kFirstColumn),
                               sources.integer(kSecondColumn),
                               sources.text(kThirdColumn)});
  }

  for (std::size_t index = 0; index < ordered_sources.size(); ++index) {
    Statement rank(database_, "UPDATE votes SET frozen_rank=? WHERE id=?");
    rank.bind(kFirstSqlParameter, static_cast<std::int64_t>(index + 1));
    rank.bind(kSecondSqlParameter, ordered_sources[index].vote_id);
    rank.step();
  }
  Statement mark_published(database_,
                           "UPDATE stories SET published_at=? WHERE id=? AND "
                           "published_at IS NULL");
  mark_published.bind(kFirstSqlParameter, now);
  mark_published.bind(kSecondSqlParameter, story_id);
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
        "VALUES(?, ?, ?)");
    notification.bind(kFirstSqlParameter, ordered_sources[index].account_id);
    notification.bind(kSecondSqlParameter, message.str());
    notification.bind(kThirdSqlParameter, now);
    notification.step();
  }
  transaction.commit();
  return {{"published", std::to_string(story_id),
           std::to_string(ordered_sources.size()), format_number(score)}};
}

}  // namespace edition
