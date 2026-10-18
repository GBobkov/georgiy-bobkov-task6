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

constexpr std::size_t kMaximumLineLength = 16384;
constexpr std::size_t kMaximumRows = 10000;

void close_descriptor(int descriptor) {
    if (descriptor >= 0) {
        ::close(descriptor);
    }
}

void set_send_timeout(int descriptor) {
    const timeval timeout{2, 0};
    if (setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
        throw std::runtime_error("cannot set socket send timeout");
    }
}

}  // namespace

LineConnection::LineConnection(int descriptor) : descriptor_(descriptor) {
    if (descriptor_ < 0) {
        throw std::invalid_argument("invalid socket descriptor");
    }
    try {
        set_send_timeout(descriptor_);
    } catch (...) {
        close_descriptor(descriptor_);
        descriptor_ = -1;
        throw;
    }
}

LineConnection::~LineConnection() {
    close_descriptor(descriptor_);
}

LineConnection::LineConnection(LineConnection&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      buffered_(std::move(other.buffered_)) {}

LineConnection& LineConnection::operator=(LineConnection&& other) noexcept {
    if (this != &other) {
        close_descriptor(descriptor_);
        descriptor_ = std::exchange(other.descriptor_, -1);
        buffered_ = std::move(other.buffered_);
    }
    return *this;
}

int LineConnection::descriptor() const {
    return descriptor_;
}

bool LineConnection::send_tokens(const Tokens& tokens) {
    const std::string message = encode_tokens(tokens);
    std::size_t sent = 0;
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

ReadResult LineConnection::receive_line(int timeout_ms) {
    while (true) {
        const std::size_t newline = buffered_.find('\n');
        if (newline != std::string::npos) {
            if (newline > kMaximumLineLength) {
                throw std::runtime_error("request line is too long");
            }
            std::string line = buffered_.substr(0, newline);
            buffered_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return {ReadStatus::line, std::move(line)};
        }
        if (buffered_.size() > kMaximumLineLength) {
            throw std::runtime_error("request line is too long");
        }

        pollfd descriptor{descriptor_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, timeout_ms);
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

        char buffer[4096];
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

LineConnection connect_tcp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    const int lookup = getaddrinfo(host.c_str(), port.c_str(), &hints,
                                   &addresses);
    if (lookup != 0) {
        throw std::runtime_error(std::string("cannot resolve host: ") +
                                 gai_strerror(lookup));
    }

    int connected = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        const int descriptor =
            ::socket(address->ai_family, address->ai_socktype,
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

void send_error(LineConnection& connection, const std::string& message) {
    connection.send_tokens({"ERR", message});
}

Rows receive_rows(LineConnection& connection) {
    const ReadResult header_result = connection.receive_line(-1);
    if (header_result.status != ReadStatus::line) {
        throw std::runtime_error("connection closed while waiting for reply");
    }
    const Tokens header = parse_tokens(header_result.value);
    if (header.empty()) {
        throw std::runtime_error("empty server reply");
    }
    if (header[0] == "ERR") {
        const std::string message = header.size() > 1 ? header[1]
                                                      : "request failed";
        throw ServerError(message);
    }
    if (header.size() != 2 || header[0] != "OK") {
        throw std::runtime_error("invalid server reply");
    }
    const std::int64_t count = parse_integer(header[1]);
    if (count < 0 || static_cast<std::uint64_t>(count) > kMaximumRows) {
        throw std::runtime_error("invalid server reply size");
    }

    Rows rows;
    rows.reserve(static_cast<std::size_t>(count));
    for (std::int64_t index = 0; index < count; ++index) {
        const ReadResult row = connection.receive_line(-1);
        if (row.status != ReadStatus::line) {
            throw std::runtime_error("connection closed during server reply");
        }
        rows.push_back(parse_tokens(row.value));
    }
    return rows;
}

}  // namespace edition
