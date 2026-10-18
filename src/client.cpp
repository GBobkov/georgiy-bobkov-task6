// Interactive console for both reporting clients and editorial accounts.
#include "app.hpp"

#include "common.hpp"
#include "net.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace edition {
namespace {

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

Rows request(LineConnection& connection, const Tokens& command) {
    if (!connection.send_tokens(command)) {
        throw std::runtime_error("не удалось отправить команду серверу");
    }
    return receive_rows(connection);
}

void print_stats(const Tokens& row) {
    if (row.size() != 8) {
        std::cout << join_tokens(row, " | ") << '\n';
        return;
    }
    std::cout << "Статистика: " << row[1] << '\n'
              << "  Принято заметок: " << row[2] << '\n'
              << "  Выпущено сюжетов: " << row[3] << '\n'
              << "  Доля выпусков: " << row[4] << "%\n"
              << "  Затронуто сюжетов: " << row[5] << '\n'
              << "  Сенсации: " << row[6] << '\n'
              << "  Средний ранг: " << row[7] << '\n';
}

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

void print_rows(const std::string& action, const Rows& rows) {
    for (const Tokens& row : rows) {
        if (row.empty()) {
            continue;
        }
        if (row[0] == "receipt" && row.size() >= 7) {
            std::cout << "Квитанция: "
                      << (row[1] == "accepted"
                              ? "принята"
                              : (row[1] == "duplicate" ? "дубль" : "отказ"))
                      << "; причина: " << describe_receipt_reason(row[2])
                      << "; сюжет: " << row[3]
                      << "; доверие: " << row[4]
                      << (row[5] == "ready" ? "; порог достигнут" : "")
                      << (row[6] == "replayed" ? "; повтор квитанции" : "")
                      << '\n';
        } else if (row[0] == "stats") {
            print_stats(row);
        } else if (row[0] == "notification" && row.size() >= 3) {
            std::cout << "Поздравление #" << row[0] << " (" << row[1]
                      << "): " << row[2] << '\n';
        } else if (row[0] == "story" && row.size() >= 8) {
            std::cout << "Сюжет #" << row[1] << " | " << row[2] << " | "
                      << row[3] << " | " << row[4] << " | доверие " << row[5]
                      << " | " << row[6] << " | порог " << row[7] << '\n';
        } else if (row[0] == "source" && row.size() >= 7) {
            std::cout << "  " << row[1] << ". " << row[2] << " (доверие "
                      << row[3] << "), событие " << row[4] << ", приём "
                      << row[5] << ", заметок " << row[6] << '\n';
        } else if (row[0] == "stats" && action == "stats-all") {
            print_stats(row);
        } else if (row.size() >= 5 && action == "users") {
            std::cout << row[0] << " | роль " << row[1] << " | доверие "
                      << row[2] << " | " << row[3] << " | пароль " << row[4]
                      << '\n';
        } else if (row.size() >= 8 && action == "stories") {
            std::cout << "#" << row[0] << " | " << row[1] << " | " << row[2]
                      << " | " << row[3] << " | доверие " << row[4]
                      << " | порог " << row[5] << " | " << row[6]
                      << " | источников " << row[7] << '\n';
        } else if (row[0] == "threshold" && row.size() >= 2) {
            std::cout << "Порог выпуска: " << row[1] << '\n';
        } else if (row[0] == "half_life_seconds" && row.size() >= 2) {
            const double hours = parse_number(row[1]) / 3600.0;
            std::cout << "Период полураспада: " << hours << " ч\n";
        } else if (row[0] == "congratulations_count" && row.size() >= 2) {
            std::cout << "Число поздравлений: " << row[1] << '\n';
        } else if (row[0] == "published" && row.size() >= 4) {
            std::cout << "Сюжет #" << row[1] << " выпущен; источников: "
                      << row[2] << "; доверие при выпуске: " << row[3] << '\n';
        } else {
            std::cout << join_tokens(row, " | ") << '\n';
        }
    }
}

void show_inbox(LineConnection& connection) {
    const Rows messages = request(connection, {"INBOX"});
    for (const Tokens& message : messages) {
        if (message.size() < 3) {
            continue;
        }
        std::cout << "Поздравление (" << message[1] << "): " << message[2]
                  << '\n';
        request(connection, {"ACK", message[0]});
    }
    if (messages.empty()) {
        std::cout << "Новых сообщений нет.\n";
    }
}

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

Tokens convert_command(const Tokens& input, const std::string& role) {
    const std::string& action = input[0];
    if (action == "note") {
        if (input.size() != 5) {
            throw std::invalid_argument(
                "формат: note ID РУБРИКА ТЕМА YYYY-MM-DDTHH:MM:SSZ");
        }
        return {"NOTE", input[1], input[2], input[3], input[4]};
    }
    if (action == "stats") {
        if (input.size() == 1 && role == "client") {
            return {"STATS"};
        }
        if (input.size() == 2 && role == "editor") {
            return input[1] == "all" ? Tokens{"STATS_ALL"}
                                     : Tokens{"STATS_USER", input[1]};
        }
        throw std::invalid_argument("неверный формат команды stats");
    }
    if (action == "inbox") {
        if (input.size() != 1) {
            throw std::invalid_argument("формат: inbox");
        }
        return {"INBOX"};
    }
    if (action == "users") {
        if (input.size() != 1) {
            throw std::invalid_argument("формат: users");
        }
        return {"USERS"};
    }
    if (action == "user-add") {
        if (input.size() < 3 || input.size() > 4) {
            throw std::invalid_argument(
                "формат: user-add client LOGIN TRUST | user-add editor LOGIN");
        }
        if (input[1] == "client" && input.size() == 4) {
            return {"USER_ADD", input[1], input[2], input[3]};
        }
        if (input[1] == "editor" && input.size() == 3) {
            return {"USER_ADD", input[1], input[2], "0"};
        }
        throw std::invalid_argument("неверные аргументы user-add");
    }
    if (action == "user-rename" && input.size() == 3) {
        return {"USER_RENAME", input[1], input[2]};
    }
    if (action == "user-trust" && input.size() == 3) {
        return {"USER_TRUST", input[1], input[2]};
    }
    if (action == "user-password" && input.size() == 2) {
        return {"USER_PASSWORD", input[1]};
    }
    if (action == "user-delete" && input.size() == 2) {
        return {"USER_DELETE", input[1]};
    }
    if (action == "user-enable" && input.size() == 2) {
        return {"USER_ENABLE", input[1]};
    }
    if (action == "password" && input.size() == 1) {
        return {"PASSWORD"};
    }
    if (action == "settings" && input.size() == 1) {
        return {"SETTINGS"};
    }
    if (action == "set" && input.size() == 3) {
        if (input[1] == "threshold") {
            return {"SET", "threshold", input[2]};
        }
        if (input[1] == "half-life-hours") {
            return {"SET", "half_life_seconds",
                    std::to_string(parse_number(input[2]) * 3600.0)};
        }
        if (input[1] == "congratulations") {
            return {"SET", "congratulations_count", input[2]};
        }
        throw std::invalid_argument("неизвестный параметр set");
    }
    if (action == "stories" && input.size() == 1) {
        return {"STORIES"};
    }
    if (action == "story" && input.size() == 2) {
        return {"STORY", input[1]};
    }
    if (action == "publish" && input.size() == 2) {
        return {"PUBLISH", input[1]};
    }
    throw std::invalid_argument("неизвестная команда или неверные аргументы");
}

}  // namespace

int run_client(int argc, char** argv) {
    if (argc > 3) {
        throw std::invalid_argument("Usage: edition client [HOST] [PORT]");
    }
    const std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    const std::string port = argc >= 3 ? argv[2] : "5050";
    LineConnection connection = connect_tcp(host, port);

    std::cout << "Логин: " << std::flush;
    std::string login;
    std::getline(std::cin, login);
    const std::string password = prompt_secret("Пароль: ");
    if (!connection.send_tokens({"AUTH", login, password})) {
        throw std::runtime_error("не удалось отправить данные входа");
    }
    const Rows authentication = receive_rows(connection);
    if (authentication.empty() || authentication[0].size() < 3 ||
        authentication[0][0] != "authenticated") {
        throw std::runtime_error("сервер вернул некорректный ответ входа");
    }
    const std::string role = authentication[0][1];
    std::cout << "Вход выполнен: " << authentication[0][2] << " (" << role
              << ").\n";
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
        if (input[0] == "help") {
            print_help();
            continue;
        }
        if (input[0] == "quit") {
            try {
                print_rows("quit", request(connection, {"QUIT"}));
            } catch (const std::exception&) {
            }
            break;
        }
        if (input[0] == "password" && input.size() == 1) {
            try {
                set_password(connection, {"PASSWORD"});
            } catch (const ServerError& error) {
                std::cout << "Ошибка: " << error.what() << '\n';
            }
            continue;
        }
        if (input[0] == "user-password" && input.size() == 2) {
            try {
                set_password(connection, {"USER_PASSWORD", input[1]});
            } catch (const ServerError& error) {
                std::cout << "Ошибка: " << error.what() << '\n';
            }
            continue;
        }

        try {
            const Tokens command = convert_command(input, role);
            if (command[0] == "INBOX") {
                show_inbox(connection);
            } else {
                print_rows(input[0], request(connection, command));
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
