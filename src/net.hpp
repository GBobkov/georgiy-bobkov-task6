#pragma once

#include "common.hpp"

#include <string>
#include <stdexcept>

namespace edition {

// Owns one TCP socket and reads/writes complete quoted-token lines.
enum class ReadStatus { line, timeout, closed };

struct ReadResult {
    ReadStatus status = ReadStatus::timeout;
    std::string value;
};

class ServerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LineConnection {
public:
    explicit LineConnection(int descriptor);
    ~LineConnection();

    LineConnection(const LineConnection&) = delete;
    LineConnection& operator=(const LineConnection&) = delete;
    LineConnection(LineConnection&& other) noexcept;
    LineConnection& operator=(LineConnection&& other) noexcept;

    int descriptor() const;
    bool send_tokens(const Tokens& tokens);
    ReadResult receive_line(int timeout_ms);

private:
    int descriptor_ = -1;
    std::string buffered_;
};

LineConnection connect_tcp(const std::string& host, const std::string& port);
void send_rows(LineConnection& connection, const Rows& rows);
void send_error(LineConnection& connection, const std::string& message);
Rows receive_rows(LineConnection& connection);

}  // namespace edition
