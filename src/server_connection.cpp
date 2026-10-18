#include "server_connection.hpp"

#include <stdexcept>
#include <string>

#include "common.hpp"
#include "net.hpp"

namespace edition {
namespace {
constexpr int kAuthenticationReplyRowCount = 1;
constexpr std::size_t kActionIndex = 0;
constexpr std::size_t kFirstArgumentIndex = 1;
constexpr std::size_t kSecondArgumentIndex = 2;
constexpr std::size_t kThirdArgumentIndex = 3;
constexpr std::size_t kFourthArgumentIndex = 4;
constexpr std::size_t kOneCommandToken = 1;
constexpr std::size_t kTwoCommandTokens = 2;
constexpr std::size_t kThreeCommandTokens = 3;
constexpr std::size_t kFourCommandTokens = 4;
constexpr std::size_t kFiveCommandTokens = 5;

// Требует заданную роль; account — вошедший пользователь, role — нужная роль.
void require_role(const Account& account, const std::string& role) {
  if (account.role != role) {
    throw std::invalid_argument("недостаточно прав для этой команды");
  }
}

// Требует точное число полей; size — ожидаемое число токенов команды.
void require_size(const Tokens& command, std::size_t size) {
  if (command.size() != size) {
    throw std::invalid_argument("неверное число аргументов");
  }
}

// Проверяет права и выполняет команду через хранилище.
// store — хранилище; account — вошедший пользователь; command — токены команды.
// received_at — время получения строки в Unix-секундах; received_order —
// общий порядок приёма, назначенный до разбора и обработки команды.
// Возвращает строки ответа; изменяющие команды фиксируют результат в store.
Rows dispatch(Store& store, const Account& account, const Tokens& command,
              std::int64_t received_at, std::int64_t received_order) {
  if (command.empty()) {
    throw std::invalid_argument("пустая команда");
  }
  const std::string& action = command[kActionIndex];
  if (action == "NOTE") {
    require_role(account, "client");
    require_size(command, kFiveCommandTokens);
    const std::int64_t event_time =
        parse_utc_time(command[kFourthArgumentIndex]);
    const Submission receipt = store.submit_note(
        account.id, command[kFirstArgumentIndex], command[kSecondArgumentIndex],
        command[kThirdArgumentIndex], event_time, received_at, received_order);
    return {{"receipt", receipt.status, receipt.reason,
             std::to_string(receipt.story_id), std::to_string(receipt.trust),
             receipt.ready ? "ready" : "not-ready",
             receipt.replayed ? "replayed" : "new"}};
  }
  if (action == "STATS") {
    require_role(account, "client");
    require_size(command, kOneCommandToken);
    return store.statistics(account.id);
  }
  if (action == "STATS_USER") {
    require_role(account, "editor");
    require_size(command, kTwoCommandTokens);
    return store.statistics_for_user(command[kFirstArgumentIndex]);
  }
  if (action == "STATS_ALL") {
    require_role(account, "editor");
    require_size(command, kOneCommandToken);
    return store.all_statistics();
  }
  if (action == "INBOX") {
    require_size(command, kOneCommandToken);
    return store.pending_notifications(account.id);
  }
  if (action == "ACK") {
    require_size(command, kTwoCommandTokens);
    store.acknowledge_notification(account.id,
                                   parse_integer(command[kFirstArgumentIndex]));
    return {{"acknowledged", command[kFirstArgumentIndex]}};
  }
  if (action == "USERS") {
    require_role(account, "editor");
    require_size(command, kOneCommandToken);
    return store.list_accounts();
  }
  if (action == "USER_ADD") {
    require_role(account, "editor");
    require_size(command, kFourCommandTokens);
    const Account created = store.add_account(
        command[kFirstArgumentIndex], command[kSecondArgumentIndex],
        parse_number(command[kThirdArgumentIndex]));
    return {{"created", created.login, created.role,
             std::to_string(created.trust), "password-not-set"}};
  }
  if (action == "USER_RENAME") {
    require_role(account, "editor");
    require_size(command, kThreeCommandTokens);
    store.rename_account(command[kFirstArgumentIndex],
                         command[kSecondArgumentIndex]);
    return {{"renamed", command[kFirstArgumentIndex],
             command[kSecondArgumentIndex]}};
  }
  if (action == "USER_TRUST") {
    require_role(account, "editor");
    require_size(command, kThreeCommandTokens);
    store.set_account_trust(command[kFirstArgumentIndex],
                            parse_number(command[kSecondArgumentIndex]));
    return {{"trust-updated", command[kFirstArgumentIndex],
             command[kSecondArgumentIndex]}};
  }
  if (action == "USER_PASSWORD") {
    require_role(account, "editor");
    require_size(command, kThreeCommandTokens);
    store.set_account_password(command[kFirstArgumentIndex],
                               command[kSecondArgumentIndex]);
    return {{"password-updated", command[kFirstArgumentIndex]}};
  }
  if (action == "PASSWORD") {
    require_size(command, kTwoCommandTokens);
    store.set_own_password(account.id, command[kFirstArgumentIndex]);
    return {{"password-updated", account.login}};
  }
  if (action == "USER_DELETE") {
    require_role(account, "editor");
    require_size(command, kTwoCommandTokens);
    store.delete_account(command[kFirstArgumentIndex], account.id);
    return {{"disabled", command[kFirstArgumentIndex]}};
  }
  if (action == "USER_ENABLE") {
    require_role(account, "editor");
    require_size(command, kTwoCommandTokens);
    store.enable_account(command[kFirstArgumentIndex]);
    return {{"enabled", command[kFirstArgumentIndex]}};
  }
  if (action == "SETTINGS") {
    require_role(account, "editor");
    require_size(command, kOneCommandToken);
    const Settings current = store.settings();
    return {{"threshold", std::to_string(current.threshold)},
            {"half_life_seconds", std::to_string(current.half_life_seconds)},
            {"congratulations_count",
             std::to_string(current.congratulations_count)}};
  }
  if (action == "SET") {
    require_role(account, "editor");
    require_size(command, kThreeCommandTokens);
    store.set_setting(command[kFirstArgumentIndex],
                      parse_number(command[kSecondArgumentIndex]));
    return {{"setting-updated", command[kFirstArgumentIndex],
             command[kSecondArgumentIndex]}};
  }
  if (action == "STORIES") {
    require_role(account, "editor");
    require_size(command, kOneCommandToken);
    return store.list_stories();
  }
  if (action == "STORY") {
    require_role(account, "editor");
    require_size(command, kTwoCommandTokens);
    return store.story_details(parse_integer(command[kFirstArgumentIndex]));
  }
  if (action == "PUBLISH") {
    require_role(account, "editor");
    require_size(command, kTwoCommandTokens);
    return store.publish_story(parse_integer(command[kFirstArgumentIndex]));
  }
  throw std::invalid_argument("неизвестная команда");
}

}  // namespace

// Обслуживает одного клиента в отдельном потоке, не задерживая accept.
// descriptor передаётся LineConnection во владение; store используется
// совместно потоками, его синхронизация находится внутри хранилища.
// До AUTH разрешён только вход; порядок строки фиксируется до её обработки.
void handle_connection(int descriptor, Store& store,
                       volatile std::sig_atomic_t& stop_requested,
                       std::atomic<std::int64_t>& next_received_order) {
  LineConnection connection(descriptor);
  bool authenticated = false;
  Account account;

  while (stop_requested == 0) {
    const ReadResult received =
        connection.receive_line(kServerWaitIntervalMilliseconds);
    if (received.status == ReadStatus::timeout) {
      continue;
    }
    if (received.status == ReadStatus::closed) {
      return;
    }
    const std::int64_t received_order = next_received_order.fetch_add(1);
    const std::int64_t received_at = current_time();

    Tokens command;
    try {
      command = parse_tokens(received.value);
    } catch (const std::exception& error) {
      send_error(connection, error.what());
      continue;
    }
    if (command.empty()) {
      send_error(connection, "пустая команда");
      continue;
    }

    if (!authenticated) {
      if (command[kActionIndex] != "AUTH" ||
          command.size() != kThreeCommandTokens) {
        send_error(connection, "сначала выполните вход");
        continue;
      }
      try {
        account = store.authenticate(command[kFirstArgumentIndex],
                                     command[kSecondArgumentIndex]);
        authenticated = true;
        if (!connection.send_tokens(
                {"OK", std::to_string(kAuthenticationReplyRowCount)}) ||
            !connection.send_tokens(
                {"authenticated", account.role, account.login})) {
          return;
        }
      } catch (const std::exception&) {
        send_error(connection, "неверный логин или пароль");
      }
      continue;
    }

    if (command[kActionIndex] == "QUIT") {
      if (command.size() != kOneCommandToken) {
        send_error(connection, "неверное число аргументов");
      } else {
        send_rows(connection, {{"bye"}});
        return;
      }
      continue;
    }
    if (command[kActionIndex] == "AUTH") {
      send_error(connection, "вход уже выполнен");
      continue;
    }
    if (!store.account_is_active(account.id)) {
      send_error(connection, "учётная запись отключена");
      return;
    }

    try {
      send_rows(connection,
                dispatch(store, account, command, received_at, received_order));
    } catch (const std::exception& error) {
      send_error(connection, error.what());
    }
  }
}

}  // namespace edition
