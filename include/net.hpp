#pragma once

#include <stdexcept>
#include <string>

#include "common.hpp"

namespace edition {

inline constexpr int kInvalidSocketDescriptor = -1;

// line — получена строка, timeout — истекло ожидание, closed — сокет закрыт.
enum class ReadStatus { line, timeout, closed };

// Результат чтения одной строки; value задано только при статусе line.
struct ReadResult {
  // Определяет, получена строка, истекло ожидание или закрыт сокет.
  ReadStatus status = ReadStatus::timeout;
  // Полученная строка без завершающего перевода строки.
  std::string value;
};

// Отделяет отказ протокола от ошибок сетевого транспорта.
class ServerError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Владеет одним сокетом; перемещение передаёт владение и буфер неполной строки.
class LineConnection {
 public:
  // Принимает сокет во владение и настраивает ограничение отправки.
  // descriptor — действующий дескриптор подключённого сокета.
  explicit LineConnection(int descriptor);
  // Закрывает принадлежащий объекту сокет.
  ~LineConnection();

  // Копирование запрещено, чтобы сокет имел единственного владельца.
  LineConnection(const LineConnection&) = delete;
  LineConnection& operator=(const LineConnection&) = delete;

  // Передаёт сокет и остаток буфера; other после операции не владеет сокетом.
  LineConnection(LineConnection&& other) noexcept;
  // Освобождает текущий сокет и принимает сокет и буфер из other;
  // other становится пустым.
  LineConnection& operator=(LineConnection&& other) noexcept;

  // Возвращает дескриптор без передачи владения; после перемещения это -1.
  int descriptor() const;

  // Отправляет строку протокола целиком; tokens — поля сообщения.
  // Возвращает false при ошибке или закрытом соединении.
  bool send_tokens(const Tokens& tokens);

  // Читает одну строку; timeout_ms задаёт ожидание в миллисекундах.
  // Отрицательное значение означает ожидание без ограничения. Результат
  // различает строку, тайм-аут и закрытие сокета.
  ReadResult receive_line(int timeout_ms);

 private:
  // Дескриптор сокета; sentinel означает, что объект им не владеет.
  int descriptor_ = kInvalidSocketDescriptor;
  // Байты после последней выданной строки сохраняются для следующего чтения.
  std::string buffered_;
};

// Подключается к первому доступному адресу; при неудаче бросает исключение.
// host — имя или адрес узла; port — TCP-порт в строковом виде.
// Возвращает объект, владеющий новым соединением.
LineConnection connect_tcp(const std::string& host, const std::string& port);

// Передаёт кадрированный ответ и прекращает отправку при ошибке сокета.
// connection — клиентский сокет; rows — строки ответа.
void send_rows(LineConnection& connection, const Rows& rows);

// Передаёт ошибку протокола без исключения при сбое отправки.
// connection — клиентский сокет; message — текст ошибки.
void send_error(LineConnection& connection, const std::string& message);

// Читает кадрированный ответ; при ERR выбрасывает ServerError.
// connection — подключённый сокет сервера.
Rows receive_rows(LineConnection& connection);

}  // namespace edition
