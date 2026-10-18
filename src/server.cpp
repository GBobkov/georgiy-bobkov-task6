// TCP-сервер команд и создание первой учётной записи редактора.
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app.hpp"
#include "common.hpp"
#include "server_connection.hpp"
#include "store.hpp"

namespace edition {
namespace {

constexpr int kInvalidDescriptor = -1;
constexpr int kFirstOptionArgumentIndex = 1;
constexpr int kOptionValueOffset = 1;
constexpr int kMinimumTcpPort = 1;
constexpr int kMaximumTcpPort = 65535;
constexpr int kListenBacklog = 32;
constexpr int kPollDescriptorCount = 1;
constexpr int kMaximumConnections = 128;
constexpr int kFirstReceivedOrder = 1;
constexpr char kDefaultServerPort[] = "5050";

// Запрос остановки сервера; меняется также обработчиком SIGINT/SIGTERM.
volatile std::sig_atomic_t stop_requested = 0;
// Сквозной порядок строк приёма; сохраняется в заметках для ранжирования.
std::atomic<std::int64_t> next_received_order{kFirstReceivedOrder};

// Не освобождает ресурсы в контексте сигнала: это делает основной поток.
extern "C" void handle_signal(int) { stop_requested = 1; }

// RAII-обёртка единолично владеет дескриптором; invalid descriptor — пустое
// состояние.
class Descriptor {
 public:
  // Принимает дескриптор во владение; value — открытый сокет или sentinel.
  explicit Descriptor(int value = kInvalidDescriptor) : value_(value) {}
  // Закрывает дескриптор, если объект им владеет.
  ~Descriptor() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }
  // Копирование запрещено, чтобы дескриптор имел единственного владельца.
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  // Переносит дескриптор; other после операции становится пустым.
  Descriptor(Descriptor&& other) noexcept
      : value_(std::exchange(other.value_, kInvalidDescriptor)) {}
  // Закрывает прежний дескриптор и принимает владение other.
  Descriptor& operator=(Descriptor&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        ::close(value_);
      }
      value_ = std::exchange(other.value_, kInvalidDescriptor);
    }
    return *this;
  }
  // Возвращает дескриптор без передачи владения.
  int get() const { return value_; }
  // Передаёт владение вызывающему коду; объект становится пустым.
  int release() { return std::exchange(value_, kInvalidDescriptor); }

 private:
  // Принадлежащий объекту сокет либо sentinel после передачи владения.
  int value_ = kInvalidDescriptor;
};

// Параметры прослушивания и файл постоянного хранилища сервера.
struct Options {
  // Адрес локального интерфейса, на котором сервер принимает подключения.
  std::string address = "127.0.0.1";
  // TCP-порт в строковом виде; при разборе проверяется допустимый диапазон.
  std::string port = kDefaultServerPort;
  // Путь к файлу базы SQLite.
  std::string database = "edition.sqlite";
};

// Разбирает параметры сервера и проверяет порт.
// argc — число аргументов; argv — их массив с подкомандой server.
Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = kFirstOptionArgumentIndex; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--help") {
      std::cout << "Usage: edition server [--bind ADDRESS] [--port PORT] "
                   "[--db FILE]\n";
      std::exit(0);
    }
    if (index + kOptionValueOffset >= argc) {
      throw std::invalid_argument("missing value for " + option);
    }
    const std::string value = argv[++index];
    if (option == "--bind") {
      options.address = value;
    } else if (option == "--port") {
      const std::int64_t port = parse_integer(value);
      if (port < kMinimumTcpPort || port > kMaximumTcpPort) {
        throw std::invalid_argument("port must be from " +
                                    std::to_string(kMinimumTcpPort) + " to " +
                                    std::to_string(kMaximumTcpPort));
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

// Создаёт сокет прослушивания на первом доступном адресе.
// options — адрес, порт и другие параметры сервера; возвращает владеющий
// дескриптор или выбрасывает исключение, если bind не удался.
Descriptor open_listener(const Options& options) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  const int lookup = getaddrinfo(options.address.c_str(), options.port.c_str(),
                                 &hints, &addresses);
  if (lookup != 0) {
    throw std::runtime_error(std::string("cannot resolve bind address: ") +
                             gai_strerror(lookup));
  }

  int listener = kInvalidDescriptor;
  for (addrinfo* address = addresses; address != nullptr;
       address = address->ai_next) {
    const int candidate = ::socket(address->ai_family, address->ai_socktype,
                                   address->ai_protocol);
    if (candidate < 0) {
      continue;
    }
    int enabled = 1;
    setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (::bind(candidate, address->ai_addr, address->ai_addrlen) == 0 &&
        ::listen(candidate, kListenBacklog) == 0) {
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

// Создаёт первую учётную запись редактора, если активных редакторов нет.
// store — постоянное хранилище; пароль вводится и проверяется интерактивно.
void bootstrap_editor(Store& store) {
  if (store.has_active_editor()) {
    return;
  }
  std::cout << "Создание первой учётной записи редактора.\n";
  std::cout << "Логин: " << std::flush;
  std::string login;
  std::getline(std::cin, login);
  if (!is_safe_field(login, limits::kMaximumLoginLength)) {
    throw std::invalid_argument(
        "login must have " + std::to_string(limits::kMinimumLoginLength) +
        " to " + std::to_string(limits::kMaximumLoginLength) + " characters");
  }
  const std::string password = prompt_secret(
      "Пароль (не менее " + std::to_string(limits::kMinimumPasswordLength) +
      " знаков): ");
  const std::string confirmation = prompt_secret("Повторите пароль: ");
  if (password != confirmation) {
    throw std::invalid_argument("passwords do not match");
  }
  if (password.size() < limits::kMinimumPasswordLength ||
      !is_safe_field(password, limits::kMaximumPasswordLength)) {
    throw std::invalid_argument(
        "password must have " + std::to_string(limits::kMinimumPasswordLength) +
        " to " + std::to_string(limits::kMaximumPasswordLength) +
        " printable characters");
  }
  const Account editor = store.add_account("editor", login, 0.0);
  store.set_account_password(login, password);
  std::cout << "Создан редактор " << editor.login << ".\n";
}

// Рабочий поток клиента и его состояние для неблокирующего ожидания.
struct Worker {
  std::thread thread;
  // Атомарный флаг выставляется потоком после выхода из обработчика.
  std::shared_ptr<std::atomic_bool> finished;
};

// Присоединяет и удаляет завершённые потоки, не блокируя приём клиентов.
// workers — список активных и завершившихся обработчиков.
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

// При остановке сервера дожидается всех потоков, оставшихся в workers.
class WorkerJoiner {
 public:
  // workers — список потоков, которыми владеет основной цикл сервера.
  explicit WorkerJoiner(std::vector<Worker>& workers) : workers_(workers) {}

  // Запрашивает остановку обработчиков и присоединяет рабочие потоки.
  ~WorkerJoiner() {
    stop_requested = 1;
    join_all();
  }

  // Дожидается потоков, которые ещё не были присоединены.
  void join_all() {
    for (Worker& worker : workers_) {
      if (worker.thread.joinable()) {
        worker.thread.join();
      }
    }
  }

 private:
  // Ссылка на список, которым управляет основной цикл сервера.
  std::vector<Worker>& workers_;
};

}  // namespace

// argc и argv — аргументы режима server; принимает подключения до сигнала
// остановки и дожидается завершения рабочих потоков.
int run_server(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  Store store(options.database);
  store.seed_initial_clients();
  bootstrap_editor(store);
  // После перезапуска нумерация продолжается за последним
  // сохранённым голосом.
  next_received_order.store(store.last_received_order() + kFirstReceivedOrder);

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
    const int ready = ::poll(&descriptor, kPollDescriptorCount,
                             kServerWaitIntervalMilliseconds);
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
    // Рабочая лямбда отмечает завершение, чтобы цикл мог безопасно
    // присоединить поток, не ожидая его в момент приёма подключений.
    std::thread connection_thread([client = accepted.get(), &store, finished] {
      try {
        handle_connection(client, store, stop_requested, next_received_order);
      } catch (const std::exception& error) {
        std::cerr << "Ошибка соединения: " << error.what() << '\n';
      }
      finished->store(true);
    });
    // Дескриптор теперь закрывает LineConnection рабочего потока.
    accepted.release();
    workers.push_back({std::move(connection_thread), std::move(finished)});
  }

  listener = Descriptor{};
  joiner.join_all();
  std::cout << "Сервер остановлен.\n";
  return 0;
}

}  // namespace edition
