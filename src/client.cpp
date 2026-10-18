// Консольный интерфейс для клиентов-источников и редакции.
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

#include "app.hpp"
#include "common.hpp"
#include "net.hpp"

namespace edition {
namespace {

constexpr std::size_t kFirstFieldIndex = 0;
constexpr std::size_t kSecondFieldIndex = 1;
constexpr std::size_t kThirdFieldIndex = 2;
constexpr std::size_t kFourthFieldIndex = 3;
constexpr std::size_t kFifthFieldIndex = 4;
constexpr std::size_t kSixthFieldIndex = 5;
constexpr std::size_t kSeventhFieldIndex = 6;
constexpr std::size_t kEighthFieldIndex = 7;

constexpr std::size_t kOneToken = 1;
constexpr std::size_t kTwoTokens = 2;
constexpr std::size_t kThreeTokens = 3;
constexpr std::size_t kFourTokens = 4;
constexpr std::size_t kFiveTokens = 5;
constexpr std::size_t kSevenTokens = 7;
constexpr std::size_t kEightTokens = 8;

constexpr int kHostArgumentIndex = 1;
constexpr int kPortArgumentIndex = 2;
constexpr int kHostArgumentCount = 2;
constexpr int kPortArgumentCount = 3;
constexpr int kMaximumClientArgumentCount = kPortArgumentCount;
constexpr double kSecondsPerMinute = 60.0;
constexpr double kMinutesPerHour = 60.0;
constexpr double kSecondsPerHour = kSecondsPerMinute * kMinutesPerHour;
constexpr char kDefaultServerPort[] = "5050";

// Печатает подсказку с командами и правилами ввода.
void print_help() {
  std::cout
      << "Команды клиента:\n"
      << "  note ID РУБРИКА ТЕМА YYYY-MM-DDTHH:MM:SSZ\n"
      << "  stats                         личная статистика\n"
      << "  inbox                         получить поздравления\n"
      << "  password                      сменить свой пароль\n"
      << "  users                         список учётных записей (редактор)\n"
      << "  user-add client LOGIN TRUST   добавить клиента\n"
      << "  user-add editor LOGIN         добавить редактора\n"
      << "  user-rename LOGIN NEW_LOGIN   изменить логин\n"
      << "  user-trust LOGIN TRUST        изменить доверие\n"
      << "  user-password LOGIN           задать новый пароль\n"
      << "  user-delete LOGIN             отключить учётную запись\n"
      << "  user-enable LOGIN             включить учётную запись\n"
      << "  settings                      параметры (редактор)\n"
      << "  set threshold VALUE           порог публикации\n"
      << "  set half-life-hours VALUE     период полураспада\n"
      << "  set congratulations COUNT     число поздравлений\n"
      << "  stories                       открытые сюжеты и выпуски\n"
      << "  story ID                      источники сюжета\n"
      << "  publish ID                    выпустить готовый сюжет\n"
      << "  stats LOGIN | stats all       статистика (редактор)\n"
      << "  help                          показать команды\n"
      << "  quit                          отключиться\n"
      << "Строки с пробелами заключайте в двойные кавычки.\n";
}

// Отправляет команду и читает ответ; отказ сервера пробрасывается как
// ServerError, чтобы консоль могла продолжить работу.
// connection — активное соединение; command — поля команды протокола.
Rows request(LineConnection& connection, const Tokens& command) {
  if (!connection.send_tokens(command)) {
    throw std::runtime_error("не удалось отправить команду серверу");
  }
  return receive_rows(connection);
}

// Печатает строку статистики или её исходные поля при неизвестном формате.
// row — строка ответа сервера.
void print_stats(const Tokens& row) {
  if (row.size() != kEightTokens) {
    std::cout << join_tokens(row, " | ") << '\n';
    return;
  }
  std::cout << "Статистика: " << row[kSecondFieldIndex] << '\n'
            << "  Принято заметок: " << row[kThirdFieldIndex] << '\n'
            << "  Выпущено сюжетов: " << row[kFourthFieldIndex] << '\n'
            << "  Доля выпусков: " << row[kFifthFieldIndex] << "%\n"
            << "  Затронуто сюжетов: " << row[kSixthFieldIndex] << '\n'
            << "  Сенсации: " << row[kSeventhFieldIndex] << '\n'
            << "  Средний ранг: " << row[kEighthFieldIndex] << '\n';
}

// Преобразует код причины квитанции в текст для пользователя.
// reason — код из ответа сервера.
std::string describe_receipt_reason(const std::string& reason) {
  if (reason == "first_source") {
    return "первый голос источника";
  }
  if (reason == "already_counted") {
    return "голос этого источника уже учтён";
  }
  if (reason == "request_already_received") {
    return "этот запрос уже был принят";
  }
  if (reason == "request_id_conflict") {
    return "ID запроса уже использован с другими данными";
  }
  if (reason == "story_published") {
    return "сюжет уже выпущен";
  }
  return reason;
}

// Форматирует строки ответа по типу действия.
// action — локальная команда; rows — строки ответа сервера.
void print_rows(const std::string& action, const Rows& rows) {
  for (const Tokens& row : rows) {
    if (row.empty()) {
      continue;
    }
    if (row[kFirstFieldIndex] == "receipt" && row.size() >= kSevenTokens) {
      std::cout
          << "Квитанция: "
          << (row[kSecondFieldIndex] == "accepted"
                  ? "принята"
                  : (row[kSecondFieldIndex] == "duplicate" ? "дубль" : "отказ"))
          << "; причина: " << describe_receipt_reason(row[kThirdFieldIndex])
          << "; сюжет: " << row[kFourthFieldIndex]
          << "; доверие: " << row[kFifthFieldIndex]
          << (row[kSixthFieldIndex] == "ready" ? "; порог достигнут" : "")
          << (row[kSeventhFieldIndex] == "replayed" ? "; повтор квитанции" : "")
          << '\n';
    } else if (row[kFirstFieldIndex] == "stats") {
      print_stats(row);
    } else if (row[kFirstFieldIndex] == "notification" &&
               row.size() >= kThreeTokens) {
      std::cout << "Поздравление #" << row[kFirstFieldIndex] << " ("
                << row[kSecondFieldIndex] << "): " << row[kThirdFieldIndex]
                << '\n';
    } else if (row[kFirstFieldIndex] == "story" && row.size() >= kEightTokens) {
      std::cout << "Сюжет #" << row[kSecondFieldIndex] << " | "
                << row[kThirdFieldIndex] << " | " << row[kFourthFieldIndex]
                << " | " << row[kFifthFieldIndex] << " | доверие "
                << row[kSixthFieldIndex] << " | " << row[kSeventhFieldIndex]
                << " | порог " << row[kEighthFieldIndex] << '\n';
    } else if (row[kFirstFieldIndex] == "source" &&
               row.size() >= kSevenTokens) {
      std::cout << "  " << row[kSecondFieldIndex] << ". "
                << row[kThirdFieldIndex] << " (доверие "
                << row[kFourthFieldIndex] << "), событие "
                << row[kFifthFieldIndex] << ", приём " << row[kSixthFieldIndex]
                << ", заметок " << row[kSeventhFieldIndex] << '\n';
    } else if (row[kFirstFieldIndex] == "stats" && action == "stats-all") {
      print_stats(row);
    } else if (row.size() >= kFiveTokens && action == "users") {
      std::cout << row[kFirstFieldIndex] << " | роль " << row[kSecondFieldIndex]
                << " | доверие " << row[kThirdFieldIndex] << " | "
                << row[kFourthFieldIndex] << " | пароль "
                << row[kFifthFieldIndex] << '\n';
    } else if (row.size() >= kEightTokens && action == "stories") {
      std::cout << "#" << row[kFirstFieldIndex] << " | "
                << row[kSecondFieldIndex] << " | " << row[kThirdFieldIndex]
                << " | " << row[kFourthFieldIndex] << " | доверие "
                << row[kFifthFieldIndex] << " | порог " << row[kSixthFieldIndex]
                << " | " << row[kSeventhFieldIndex] << " | источников "
                << row[kEighthFieldIndex] << '\n';
    } else if (row[kFirstFieldIndex] == "threshold" &&
               row.size() >= kTwoTokens) {
      std::cout << "Порог выпуска: " << row[kSecondFieldIndex] << '\n';
    } else if (row[kFirstFieldIndex] == "half_life_seconds" &&
               row.size() >= kTwoTokens) {
      const double hours =
          parse_number(row[kSecondFieldIndex]) / kSecondsPerHour;
      std::cout << "Период полураспада: " << hours << " ч\n";
    } else if (row[kFirstFieldIndex] == "congratulations_count" &&
               row.size() >= kTwoTokens) {
      std::cout << "Число поздравлений: " << row[kSecondFieldIndex] << '\n';
    } else if (row[kFirstFieldIndex] == "published" &&
               row.size() >= kFourTokens) {
      std::cout << "Сюжет #" << row[kSecondFieldIndex]
                << " выпущен; источников: " << row[kThirdFieldIndex]
                << "; доверие при выпуске: " << row[kFourthFieldIndex] << '\n';
    } else {
      std::cout << join_tokens(row, " | ") << '\n';
    }
  }
}

// Показывает очередь и подтверждает сообщение только после его вывода.
// При потере соединения неподтверждённое сообщение будет доставлено повторно.
// connection — соединение вошедшего пользователя.
void show_inbox(LineConnection& connection) {
  const Rows messages = request(connection, {"INBOX"});
  for (const Tokens& message : messages) {
    if (message.size() < kThreeTokens) {
      continue;
    }
    std::cout << "Поздравление (" << message[kSecondFieldIndex]
              << "): " << message[kThirdFieldIndex] << '\n';
    request(connection, {"ACK", message[kFirstFieldIndex]});
  }
  if (messages.empty()) {
    std::cout << "Новых сообщений нет.\n";
  }
}

// Запрашивает пароль дважды и не отправляет его при несовпадении.
// connection — активное соединение; command — префикс команды смены пароля.
void set_password(LineConnection& connection, const Tokens& command) {
  const std::string first = prompt_secret("Новый пароль: ");
  const std::string second = prompt_secret("Повторите пароль: ");
  if (first != second) {
    throw std::invalid_argument("пароли не совпадают");
  }
  Tokens request_tokens = command;
  request_tokens.push_back(first);
  print_rows("password", request(connection, request_tokens));
}

// Преобразует команду интерфейса в формат протокола.
// input — разобранная команда; role — роль пользователя для выбора синтаксиса.
// Полномочия повторно проверяются сервером.
Tokens convert_command(const Tokens& input, const std::string& role) {
  const std::string& action = input[kFirstFieldIndex];
  if (action == "note") {
    if (input.size() != kFiveTokens) {
      throw std::invalid_argument(
          "формат: note ID РУБРИКА ТЕМА YYYY-MM-DDTHH:MM:SSZ");
    }
    return {"NOTE", input[kSecondFieldIndex], input[kThirdFieldIndex],
            input[kFourthFieldIndex], input[kFifthFieldIndex]};
  }
  if (action == "stats") {
    if (input.size() == kOneToken && role == "client") {
      return {"STATS"};
    }
    if (input.size() == kTwoTokens && role == "editor") {
      return input[kSecondFieldIndex] == "all"
                 ? Tokens{"STATS_ALL"}
                 : Tokens{"STATS_USER", input[kSecondFieldIndex]};
    }
    throw std::invalid_argument("неверный формат команды stats");
  }
  if (action == "inbox") {
    if (input.size() != kOneToken) {
      throw std::invalid_argument("формат: inbox");
    }
    return {"INBOX"};
  }
  if (action == "users") {
    if (input.size() != kOneToken) {
      throw std::invalid_argument("формат: users");
    }
    return {"USERS"};
  }
  if (action == "user-add") {
    if (input.size() < kThreeTokens || input.size() > kFourTokens) {
      throw std::invalid_argument(
          "формат: user-add client LOGIN TRUST | user-add editor LOGIN");
    }
    if (input[kSecondFieldIndex] == "client" && input.size() == kFourTokens) {
      return {"USER_ADD", input[kSecondFieldIndex], input[kThirdFieldIndex],
              input[kFourthFieldIndex]};
    }
    if (input[kSecondFieldIndex] == "editor" && input.size() == kThreeTokens) {
      return {"USER_ADD", input[kSecondFieldIndex], input[kThirdFieldIndex],
              "0"};
    }
    throw std::invalid_argument("неверные аргументы user-add");
  }
  if (action == "user-rename" && input.size() == kThreeTokens) {
    return {"USER_RENAME", input[kSecondFieldIndex], input[kThirdFieldIndex]};
  }
  if (action == "user-trust" && input.size() == kThreeTokens) {
    return {"USER_TRUST", input[kSecondFieldIndex], input[kThirdFieldIndex]};
  }
  if (action == "user-password" && input.size() == kTwoTokens) {
    return {"USER_PASSWORD", input[kSecondFieldIndex]};
  }
  if (action == "user-delete" && input.size() == kTwoTokens) {
    return {"USER_DELETE", input[kSecondFieldIndex]};
  }
  if (action == "user-enable" && input.size() == kTwoTokens) {
    return {"USER_ENABLE", input[kSecondFieldIndex]};
  }
  if (action == "password" && input.size() == kOneToken) {
    return {"PASSWORD"};
  }
  if (action == "settings" && input.size() == kOneToken) {
    return {"SETTINGS"};
  }
  if (action == "set" && input.size() == kThreeTokens) {
    if (input[kSecondFieldIndex] == "threshold") {
      return {"SET", "threshold", input[kThirdFieldIndex]};
    }
    if (input[kSecondFieldIndex] == "half-life-hours") {
      return {"SET", "half_life_seconds",
              std::to_string(parse_number(input[kThirdFieldIndex]) *
                             kSecondsPerHour)};
    }
    if (input[kSecondFieldIndex] == "congratulations") {
      return {"SET", "congratulations_count", input[kThirdFieldIndex]};
    }
    throw std::invalid_argument("неизвестный параметр set");
  }
  if (action == "stories" && input.size() == kOneToken) {
    return {"STORIES"};
  }
  if (action == "story" && input.size() == kTwoTokens) {
    return {"STORY", input[kSecondFieldIndex]};
  }
  if (action == "publish" && input.size() == kTwoTokens) {
    return {"PUBLISH", input[kSecondFieldIndex]};
  }
  throw std::invalid_argument("неизвестная команда или неверные аргументы");
}

}  // namespace

// argc и argv — аргументы режима client; подключается, проходит авторизацию
// и обслуживает команды до выхода пользователя.
int run_client(int argc, char** argv) {
  if (argc > kMaximumClientArgumentCount) {
    throw std::invalid_argument("Usage: edition client [HOST] [PORT]");
  }
  const std::string host =
      argc >= kHostArgumentCount ? argv[kHostArgumentIndex] : "127.0.0.1";
  const std::string port = argc >= kPortArgumentCount ? argv[kPortArgumentIndex]
                                                      : kDefaultServerPort;
  LineConnection connection = connect_tcp(host, port);

  std::cout << "Логин: " << std::flush;
  std::string login;
  std::getline(std::cin, login);
  const std::string password = prompt_secret("Пароль: ");
  if (!connection.send_tokens({"AUTH", login, password})) {
    throw std::runtime_error("не удалось отправить данные входа");
  }
  const Rows authentication = receive_rows(connection);
  if (authentication.empty() ||
      authentication[kFirstFieldIndex].size() < kThreeTokens ||
      authentication[kFirstFieldIndex][kFirstFieldIndex] != "authenticated") {
    throw std::runtime_error("сервер вернул некорректный ответ входа");
  }
  const std::string role = authentication[kFirstFieldIndex][kSecondFieldIndex];
  std::cout << "Вход выполнен: "
            << authentication[kFirstFieldIndex][kThirdFieldIndex] << " ("
            << role << ").\n";
  show_inbox(connection);

  std::string line;
  while (true) {
    std::cout << "edition> " << std::flush;
    if (!std::getline(std::cin, line)) {
      break;
    }
    Tokens input;
    try {
      input = parse_tokens(line);
    } catch (const std::exception& error) {
      std::cout << "Ошибка: " << error.what() << '\n';
      continue;
    }
    if (input.empty()) {
      continue;
    }
    if (input[kFirstFieldIndex] == "help") {
      print_help();
      continue;
    }
    if (input[kFirstFieldIndex] == "quit") {
      try {
        print_rows("quit", request(connection, {"QUIT"}));
      } catch (const std::exception&) {
      }
      break;
    }
    if (input[kFirstFieldIndex] == "password" && input.size() == kOneToken) {
      try {
        set_password(connection, {"PASSWORD"});
      } catch (const ServerError& error) {
        std::cout << "Ошибка: " << error.what() << '\n';
      }
      continue;
    }
    if (input[kFirstFieldIndex] == "user-password" &&
        input.size() == kTwoTokens) {
      try {
        set_password(connection, {"USER_PASSWORD", input[kSecondFieldIndex]});
      } catch (const ServerError& error) {
        std::cout << "Ошибка: " << error.what() << '\n';
      }
      continue;
    }

    try {
      const Tokens command = convert_command(input, role);
      if (command[kFirstFieldIndex] == "INBOX") {
        show_inbox(connection);
      } else {
        print_rows(input[kFirstFieldIndex], request(connection, command));
      }
    } catch (const ServerError& error) {
      std::cout << "Ошибка: " << error.what() << '\n';
    } catch (const std::exception& error) {
      std::cout << "Ошибка: " << error.what() << '\n';
    }
  }
  return 0;
}

}  // namespace edition
