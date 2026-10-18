// TCP command server and the first-run editor bootstrap flow.
#include "app.hpp"

#include "common.hpp"
#include "net.hpp"
#include "store.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace edition {
namespace {

volatile std::sig_atomic_t stop_requested = 0;
std::atomic<std::int64_t> next_received_order{1};
constexpr int kMaximumConnections = 128;

extern "C" void handle_signal(int) {
    stop_requested = 1;
}

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                ::close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const {
        return value_;
    }
    int release() {
        return std::exchange(value_, -1);
    }

private:
    int value_ = -1;
};

struct Options {
    std::string address = "127.0.0.1";
    std::string port = "5050";
    std::string database = "edition.sqlite";
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help") {
            std::cout << "Usage: edition server [--bind ADDRESS] [--port PORT] "
                         "[--db FILE]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + option);
        }
        const std::string value = argv[++index];
        if (option == "--bind") {
            options.address = value;
        } else if (option == "--port") {
            const std::int64_t port = parse_integer(value);
            if (port < 1 || port > 65535) {
                throw std::invalid_argument("port must be from 1 to 65535");
            }
            options.port = value;
        } else if (option == "--db") {
            options.database = value;
        } else {
            throw std::invalid_argument("unknown server option: " + option);
        }
    }
    return options;
}

Descriptor open_listener(const Options& options) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    const int lookup =
        getaddrinfo(options.address.c_str(), options.port.c_str(), &hints,
                    &addresses);
    if (lookup != 0) {
        throw std::runtime_error(std::string("cannot resolve bind address: ") +
                                 gai_strerror(lookup));
    }

    int listener = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        const int candidate =
            ::socket(address->ai_family, address->ai_socktype,
                     address->ai_protocol);
        if (candidate < 0) {
            continue;
        }
        int enabled = 1;
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled));
        if (::bind(candidate, address->ai_addr, address->ai_addrlen) == 0 &&
            ::listen(candidate, 32) == 0) {
            listener = candidate;
            break;
        }
        ::close(candidate);
    }
    freeaddrinfo(addresses);
    if (listener < 0) {
        throw std::runtime_error("could not bind server socket");
    }
    return Descriptor(listener);
}

void bootstrap_editor(Store& store) {
    if (store.has_active_editor()) {
        return;
    }
    std::cout << "Создание первой учётной записи редактора.\n";
    std::cout << "Логин: " << std::flush;
    std::string login;
    std::getline(std::cin, login);
    if (!is_safe_field(login, 128)) {
        throw std::invalid_argument("login must have 1 to 128 characters");
    }
    const std::string password = prompt_secret("Пароль (не менее 8 знаков): ");
    const std::string confirmation = prompt_secret("Повторите пароль: ");
    if (password != confirmation) {
        throw std::invalid_argument("passwords do not match");
    }
    if (password.size() < 8 || !is_safe_field(password, 1024)) {
        throw std::invalid_argument(
            "password must have 8 to 1024 printable characters");
    }
    const Account editor = store.add_account("editor", login, 0.0);
    store.set_account_password(login, password);
    std::cout << "Создан редактор " << editor.login << ".\n";
}

void require_role(const Account& account, const std::string& role) {
    if (account.role != role) {
        throw std::invalid_argument("недостаточно прав для этой команды");
    }
}

void require_size(const Tokens& command, std::size_t size) {
    if (command.size() != size) {
        throw std::invalid_argument("неверное число аргументов");
    }
}

Rows dispatch(Store& store, const Account& account, const Tokens& command,
              std::int64_t received_at, std::int64_t received_order) {
    if (command.empty()) {
        throw std::invalid_argument("пустая команда");
    }
    const std::string& action = command[0];
    if (action == "NOTE") {
        require_role(account, "client");
        require_size(command, 5);
        const std::int64_t event_time = parse_utc_time(command[4]);
        const Submission receipt = store.submit_note(
            account.id, command[1], command[2], command[3], event_time,
            received_at, received_order);
        return {{"receipt", receipt.status, receipt.reason,
                 std::to_string(receipt.story_id),
                 std::to_string(receipt.trust),
                 receipt.ready ? "ready" : "not-ready",
                 receipt.replayed ? "replayed" : "new"}};
    }
    if (action == "STATS") {
        require_role(account, "client");
        require_size(command, 1);
        return store.statistics(account.id);
    }
    if (action == "STATS_USER") {
        require_role(account, "editor");
        require_size(command, 2);
        return store.statistics_for_user(command[1]);
    }
    if (action == "STATS_ALL") {
        require_role(account, "editor");
        require_size(command, 1);
        return store.all_statistics();
    }
    if (action == "INBOX") {
        require_size(command, 1);
        return store.pending_notifications(account.id);
    }
    if (action == "ACK") {
        require_size(command, 2);
        store.acknowledge_notification(account.id, parse_integer(command[1]));
        return {{"acknowledged", command[1]}};
    }
    if (action == "USERS") {
        require_role(account, "editor");
        require_size(command, 1);
        return store.list_accounts();
    }
    if (action == "USER_ADD") {
        require_role(account, "editor");
        require_size(command, 4);
        const Account created = store.add_account(
            command[1], command[2], parse_number(command[3]));
        return {{"created", created.login, created.role,
                 std::to_string(created.trust), "password-not-set"}};
    }
    if (action == "USER_RENAME") {
        require_role(account, "editor");
        require_size(command, 3);
        store.rename_account(command[1], command[2]);
        return {{"renamed", command[1], command[2]}};
    }
    if (action == "USER_TRUST") {
        require_role(account, "editor");
        require_size(command, 3);
        store.set_account_trust(command[1], parse_number(command[2]));
        return {{"trust-updated", command[1], command[2]}};
    }
    if (action == "USER_PASSWORD") {
        require_role(account, "editor");
        require_size(command, 3);
        store.set_account_password(command[1], command[2]);
        return {{"password-updated", command[1]}};
    }
    if (action == "PASSWORD") {
        require_size(command, 2);
        store.set_own_password(account.id, command[1]);
        return {{"password-updated", account.login}};
    }
    if (action == "USER_DELETE") {
        require_role(account, "editor");
        require_size(command, 2);
        store.delete_account(command[1], account.id);
        return {{"disabled", command[1]}};
    }
    if (action == "USER_ENABLE") {
        require_role(account, "editor");
        require_size(command, 2);
        store.enable_account(command[1]);
        return {{"enabled", command[1]}};
    }
    if (action == "SETTINGS") {
        require_role(account, "editor");
        require_size(command, 1);
        const Settings current = store.settings();
        return {{"threshold", std::to_string(current.threshold)},
                {"half_life_seconds",
                 std::to_string(current.half_life_seconds)},
                {"congratulations_count",
                 std::to_string(current.congratulations_count)}};
    }
    if (action == "SET") {
        require_role(account, "editor");
        require_size(command, 3);
        store.set_setting(command[1], parse_number(command[2]));
        return {{"setting-updated", command[1], command[2]}};
    }
    if (action == "STORIES") {
        require_role(account, "editor");
        require_size(command, 1);
        return store.list_stories();
    }
    if (action == "STORY") {
        require_role(account, "editor");
        require_size(command, 2);
        return store.story_details(parse_integer(command[1]));
    }
    if (action == "PUBLISH") {
        require_role(account, "editor");
        require_size(command, 2);
        return store.publish_story(parse_integer(command[1]));
    }
    throw std::invalid_argument("неизвестная команда");
}

// Each connection has its own worker; idle or slow clients do not block accept.
void handle_connection(int descriptor, Store& store) {
    LineConnection connection(descriptor);
    bool authenticated = false;
    Account account;

    while (stop_requested == 0) {
        const ReadResult received = connection.receive_line(500);
        if (received.status == ReadStatus::timeout) {
            continue;
        }
        if (received.status == ReadStatus::closed) {
            return;
        }
        const std::int64_t received_order =
            next_received_order.fetch_add(1);
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
            if (command[0] != "AUTH" || command.size() != 3) {
                send_error(connection, "сначала выполните вход");
                continue;
            }
            try {
                account = store.authenticate(command[1], command[2]);
                authenticated = true;
                if (!connection.send_tokens(
                        {"OK", "1"}) ||
                    !connection.send_tokens(
                        {"authenticated", account.role, account.login})) {
                    return;
                }
            } catch (const std::exception&) {
                send_error(connection, "неверный логин или пароль");
            }
            continue;
        }

        if (command[0] == "QUIT") {
            if (command.size() != 1) {
                send_error(connection, "неверное число аргументов");
            } else {
                send_rows(connection, {{"bye"}});
                return;
            }
            continue;
        }
        if (command[0] == "AUTH") {
            send_error(connection, "вход уже выполнен");
            continue;
        }
        if (!store.account_is_active(account.id)) {
            send_error(connection, "учётная запись отключена");
            return;
        }

        try {
            send_rows(connection,
                      dispatch(store, account, command, received_at,
                               received_order));
        } catch (const std::exception& error) {
            send_error(connection, error.what());
        }
    }
}

struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> finished;
};

void reap_workers(std::vector<Worker>& workers) {
    auto iterator = workers.begin();
    while (iterator != workers.end()) {
        if (iterator->finished->load()) {
            iterator->thread.join();
            iterator = workers.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

class WorkerJoiner {
public:
    explicit WorkerJoiner(std::vector<Worker>& workers) : workers_(workers) {}

    ~WorkerJoiner() {
        stop_requested = 1;
        join_all();
    }

    void join_all() {
        for (Worker& worker : workers_) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
        }
    }

private:
    std::vector<Worker>& workers_;
};

}  // namespace

int run_server(int argc, char** argv) {
    const Options options = parse_options(argc, argv);
    Store store(options.database);
    store.seed_initial_clients();
    bootstrap_editor(store);
    next_received_order.store(store.last_received_order() + 1);

    stop_requested = 0;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    Descriptor listener = open_listener(options);
    std::cout << "Сервер слушает " << options.address << ':' << options.port
              << " и хранит данные в " << options.database << ".\n";

    std::vector<Worker> workers;
    workers.reserve(kMaximumConnections);
    WorkerJoiner joiner(workers);
    while (stop_requested == 0) {
        reap_workers(workers);
        pollfd descriptor{listener.get(), POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("server socket poll failed");
        }
        if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }

        const int client = ::accept(listener.get(), nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error("could not accept client connection");
        }
        if (workers.size() >= kMaximumConnections) {
            ::close(client);
            continue;
        }

        Descriptor accepted(client);
        auto finished = std::make_shared<std::atomic_bool>(false);
        std::thread connection_thread([client = accepted.get(), &store,
                                       finished] {
            try {
                handle_connection(client, store);
            } catch (const std::exception& error) {
                std::cerr << "Ошибка соединения: " << error.what() << '\n';
            }
            finished->store(true);
        });
        accepted.release();
        workers.push_back(
            {std::move(connection_thread), std::move(finished)});
    }

    listener = Descriptor{};
    joiner.join_all();
    std::cout << "Сервер остановлен.\n";
    return 0;
}

}  // namespace edition
