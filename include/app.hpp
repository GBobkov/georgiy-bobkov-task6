#pragma once

namespace edition {

// Запускает сервер.
// argc — число аргументов; argv — массив аргументов, включая "server".
// Возвращает код завершения или выбрасывает исключение при ошибке запуска.
int run_server(int argc, char** argv);

// Запускает клиент.
// argc — число аргументов; argv — массив аргументов, включая "client".
// Возвращает код завершения или выбрасывает исключение при ошибке соединения.
int run_client(int argc, char** argv);

}  // namespace edition
