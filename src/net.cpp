#include "net.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace edition {
namespace {

// Ограничения кадра: длина строки в байтах и число строк в ответе.
constexpr std::size_t kMaximumLineLength = 16384;
constexpr std::size_t kMaximumRows = 10000;
constexpr int kSocketSendTimeoutSeconds = 2;
constexpr std::size_t kReceiveBufferLength = 4096;
constexpr int kPollDescriptorCount = 1;
constexpr int kWaitWithoutTimeout = -1;
constexpr std::size_t kFirstResponseFieldIndex = 0;
constexpr std::size_t kSecondResponseFieldIndex = 1;
constexpr std::size_t kResponseHeaderFieldCount = 2;
constexpr std::size_t kNextLineOffset = 1;

// Закрывает открытый сокет; invalid descriptor допустим.
void close_descriptor(int descriptor) {
  if (descriptor >= 0) {
    ::close(descriptor);
  }
}

// Ограничивает отправку двумя секундами; descriptor — сокет соединения.
void set_send_timeout(int descriptor) {
  const timeval timeout{kSocketSendTimeoutSeconds, 0};
  if (setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    throw std::runtime_error("cannot set socket send timeout");
  }
}

}  // namespace

// Принимает сокет во владение и настраивает ограничение времени отправки;
// descriptor должен быть открытым файловым дескриптором.
LineConnection::LineConnection(int descriptor) : descriptor_(descriptor) {
  if (descriptor_ < 0) {
    throw std::invalid_argument("invalid socket descriptor");
  }
  try {
    set_send_timeout(descriptor_);
  } catch (...) {
    close_descriptor(descriptor_);
    descriptor_ = kInvalidSocketDescriptor;
    throw;
  }
}

// Закрывает принадлежащий соединению сокет, если он ещё не передан.
LineConnection::~LineConnection() { close_descriptor(descriptor_); }

// Переносит сокет и уже полученные данные; other становится пустым.
LineConnection::LineConnection(LineConnection&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, kInvalidSocketDescriptor)),
      buffered_(std::move(other.buffered_)) {}

// Закрывает текущий сокет и принимает владение сокетом other.
LineConnection& LineConnection::operator=(LineConnection&& other) noexcept {
  if (this != &other) {
    close_descriptor(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, kInvalidSocketDescriptor);
    buffered_ = std::move(other.buffered_);
  }
  return *this;
}

// Возвращает дескриптор без передачи его владения вызывающему коду.
int LineConnection::descriptor() const { return descriptor_; }

// Отправляет все закодированные поля; tokens — одна команда протокола.
// Возвращает false при разрыве соединения или ошибке записи.
bool LineConnection::send_tokens(const Tokens& tokens) {
  const std::string message = encode_tokens(tokens);
  std::size_t sent = 0;
  // send может записать только часть строки, поэтому продолжаем отправку.
  while (sent < message.size()) {
    const ssize_t count = ::send(descriptor_, message.data() + sent,
                                 message.size() - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

// Читает одну строку с тайм-аутом; timeout_ms задан в миллисекундах,
// отрицательное значение означает ожидание без ограничения.
ReadResult LineConnection::receive_line(int timeout_ms) {
  // TCP не сохраняет границы отправок: буфер может содержать часть строки
  // или несколько строк, поэтому за вызов выдаётся только первая.
  while (true) {
    const std::size_t newline = buffered_.find('\n');
    if (newline != std::string::npos) {
      if (newline > kMaximumLineLength) {
        throw std::runtime_error("request line is too long");
      }
      std::string line = buffered_.substr(0, newline);
      buffered_.erase(0, newline + kNextLineOffset);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return {ReadStatus::line, std::move(line)};
    }
    if (buffered_.size() > kMaximumLineLength) {
      throw std::runtime_error("request line is too long");
    }

    pollfd descriptor{descriptor_, POLLIN, 0};
    const int ready = ::poll(&descriptor, kPollDescriptorCount, timeout_ms);
    if (ready == 0) {
      return {ReadStatus::timeout, {}};
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("socket poll failed: ") +
                               std::strerror(errno));
    }

    char buffer[kReceiveBufferLength];
    const ssize_t count = ::recv(descriptor_, buffer, sizeof(buffer), 0);
    if (count == 0) {
      return {ReadStatus::closed, {}};
    }
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      throw std::runtime_error(std::string("socket read failed: ") +
                               std::strerror(errno));
    }
    buffered_.append(buffer, static_cast<std::size_t>(count));
  }
}

// Устанавливает TCP-соединение; host — имя или адрес узла, port — TCP-порт.
// Возвращает объект, владеющий сокетом, либо выбрасывает исключение.
LineConnection connect_tcp(const std::string& host, const std::string& port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* addresses = nullptr;
  const int lookup =
      getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses);
  if (lookup != 0) {
    throw std::runtime_error(std::string("cannot resolve host: ") +
                             gai_strerror(lookup));
  }

  int connected = kInvalidSocketDescriptor;
  for (addrinfo* address = addresses; address != nullptr;
       address = address->ai_next) {
    const int descriptor = ::socket(address->ai_family, address->ai_socktype,
                                    address->ai_protocol);
    if (descriptor < 0) {
      continue;
    }
    if (::connect(descriptor, address->ai_addr, address->ai_addrlen) == 0) {
      connected = descriptor;
      break;
    }
    close_descriptor(descriptor);
  }
  freeaddrinfo(addresses);
  if (connected < 0) {
    throw std::runtime_error("could not connect to server");
  }
  return LineConnection(connected);
}

// Передаёт ответ с числом строк и их содержимым; connection — клиентский
// сокет, rows — строки ответа.
void send_rows(LineConnection& connection, const Rows& rows) {
  if (!connection.send_tokens({"OK", std::to_string(rows.size())})) {
    return;
  }
  for (const Tokens& row : rows) {
    if (!connection.send_tokens(row)) {
      return;
    }
  }
}

// Передаёт ошибку протокола; connection — клиентский сокет, message — причина.
void send_error(LineConnection& connection, const std::string& message) {
  connection.send_tokens({"ERR", message});
}

// Читает ответ сервера и проверяет его заголовок и размер; connection —
// активное соединение. Ошибка сервера или повреждённый ответ дают исключение.
Rows receive_rows(LineConnection& connection) {
  const ReadResult header_result = connection.receive_line(kWaitWithoutTimeout);
  if (header_result.status != ReadStatus::line) {
    throw std::runtime_error("connection closed while waiting for reply");
  }
  const Tokens header = parse_tokens(header_result.value);
  if (header.empty()) {
    throw std::runtime_error("empty server reply");
  }
  if (header[kFirstResponseFieldIndex] == "ERR") {
    const std::string message =
        header.size() > kFirstResponseFieldIndex + kNextLineOffset
            ? header[kSecondResponseFieldIndex]
            : "request failed";
    throw ServerError(message);
  }
  if (header.size() != kResponseHeaderFieldCount ||
      header[kFirstResponseFieldIndex] != "OK") {
    throw std::runtime_error("invalid server reply");
  }
  const std::int64_t count = parse_integer(header[kSecondResponseFieldIndex]);
  // Размер ответа ограничен, чтобы некорректный заголовок не вызвал
  // чрезмерное выделение памяти или долгий цикл чтения.
  if (count < 0 || static_cast<std::uint64_t>(count) > kMaximumRows) {
    throw std::runtime_error("invalid server reply size");
  }

  Rows rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (std::int64_t index = 0; index < count; ++index) {
    const ReadResult row = connection.receive_line(kWaitWithoutTimeout);
    if (row.status != ReadStatus::line) {
      throw std::runtime_error("connection closed during server reply");
    }
    rows.push_back(parse_tokens(row.value));
  }
  return rows;
}

}  // namespace edition
