#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "common.hpp"

struct sqlite3;

namespace edition {

namespace settings_limits {
inline constexpr double kDefaultThreshold = 1.01;
inline constexpr std::int64_t kDefaultHalfLifeSeconds = 21600;
inline constexpr std::int64_t kDefaultCongratulationsCount = 3;
inline constexpr double kMaximumThreshold = 100000.0;
inline constexpr std::int64_t kMinimumHalfLifeSeconds = 60;
inline constexpr std::int64_t kMaximumHalfLifeSeconds = 315360000;
inline constexpr std::int64_t kMaximumCongratulationsCount = 100;
}  // namespace settings_limits

// Снимок учётной записи; ID сохраняется при переименовании логина.
struct Account {
  std::int64_t id = 0;
  std::string login;
  // Роль client или editor.
  std::string role;
  // Доверие клиента в диапазоне [0, 1]; у редактора всегда равно нулю.
  double trust = 0.0;
};

// Квитанция заметки; повтор request_id возвращает прежний итог без нового
// голоса.
struct Submission {
  // Результат приёма: accepted, duplicate или rejected.
  std::string status;
  // Причина результата в коде протокола.
  std::string reason;
  // ID сюжета; равен нулю, если запрос не связан с сюжетом.
  std::int64_t story_id = 0;
  // Доверие сюжета на момент приёма заметки.
  double trust = 0.0;
  // Был ли достигнут порог на момент приёма.
  bool ready = false;
  // Повтор уже принятого request_id, которому возвращена прежняя квитанция.
  bool replayed = false;
};

// Редакционные настройки доверия и количества отправляемых поздравлений.
struct Settings {
  // Минимальная сумма доверия задаётся между нулём и верхним пределом.
  double threshold = settings_limits::kDefaultThreshold;
  // Период полураспада в секундах в допустимом диапазоне.
  std::int64_t half_life_seconds = settings_limits::kDefaultHalfLifeSeconds;
  // Число первых источников для поздравления: целое в допустимом диапазоне.
  std::int64_t congratulations_count =
      settings_limits::kDefaultCongratulationsCount;
};

// Потокобезопасное хранилище. Владеет одним SQLite-соединением; методы
// сериализуют доступ к нему через mutex, копирование объекта запрещено.
class Store {
 public:
  // Открывает базу и применяет схему; path — путь к файлу SQLite.
  explicit Store(const std::string& path);
  // Закрывает SQLite-соединение.
  ~Store();

  // Запрещает копирование общего соединения и связанного с ним mutex.
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Создаёт заданные начальные учётные записи, не меняя существующие.
  void seed_initial_clients();

  // Возвращает true, если доступен хотя бы один редактор.
  bool has_active_editor();

  // Проверяет доступность учётной записи; account_id — её постоянный ID.
  // Возвращает true только для активной записи.
  bool account_is_active(std::int64_t account_id);

  // Возвращает максимальный сохранённый порядок приёма заметки.
  std::int64_t last_received_order();

  // Проверяет пароль и возвращает снимок активной учётной записи.
  // login — имя входа; password — пароль. При отказе бросает исключение.
  Account authenticate(const std::string& login, const std::string& password);

  // Создаёт запись без пароля и возвращает её снимок.
  // trust для редактора принудительно равен нулю.
  // role — client или editor; login — уникальное имя; trust — доверие [0, 1].
  Account add_account(const std::string& role, const std::string& login,
                      double trust);

  // Меняет имя входа, сохраняя ID и историю.
  // old_login — текущий логин; new_login — уникальное новое имя.
  void rename_account(const std::string& old_login,
                      const std::string& new_login);

  // Меняет доверие для будущих заметок.
  // login — клиент; trust — значение в диапазоне [0, 1].
  void set_account_trust(const std::string& login, double trust);

  // Задаёт пароль и сохраняет только его соль и хеш.
  // login — имя учётной записи; password — новый пароль.
  void set_account_password(const std::string& login,
                            const std::string& password);

  // Меняет собственный пароль; account_id — ID вошедшего пользователя.
  void set_own_password(std::int64_t account_id, const std::string& password);

  // Отключает вход, сохраняя историю; запрещено отключать себя или
  // последнего активного редактора. login — цель, current_account_id — автор.
  void delete_account(const std::string& login,
                      std::int64_t current_account_id);

  // Восстанавливает вход; login — имя отключённой учётной записи.
  void enable_account(const std::string& login);

  // Возвращает все учётные записи с ролью, доверием и состоянием доступа.
  Rows list_accounts();

  // Возвращает текущие настройки; отсутствующие значения заменяются
  // умолчаниями.
  Settings settings();

  // Меняет параметр после проверки диапазона.
  // name — имя настройки; value — порог доверия, период полураспада
  // в секундах или число поздравлений.
  void set_setting(const std::string& name, double value);

  // Принимает заметку идемпотентно и возвращает квитанцию.
  // account_id — источник; category и topic — точный ключ сюжета;
  // request_id — идентификатор повтора.
  // event_time — время события; received_at — время приёма,
  // оба заданы в Unix-секундах.
  // received_order — глобальный порядковый номер получения строки.
  Submission submit_note(std::int64_t account_id, const std::string& request_id,
                         const std::string& category, const std::string& topic,
                         std::int64_t event_time, std::int64_t received_at,
                         std::int64_t received_order);

  // Возвращает сюжеты с доверием, пересчитанным на текущее время.
  Rows list_stories();

  // Возвращает сюжет и всех источников в порядке приёма.
  // story_id — внутренний ID сюжета.
  Rows story_details(std::int64_t story_id);

  // Необратимо выпускает сюжет при достигнутом пороге доверия.
  // story_id — ID сюжета; возвращает сводку выпуска.
  Rows publish_story(std::int64_t story_id);

  // Возвращает статистику клиента; account_id — его постоянный ID.
  Rows statistics(std::int64_t account_id);

  // Возвращает статистику клиента; login — его имя входа.
  Rows statistics_for_user(const std::string& login);

  // Возвращает статистику всех клиентских учётных записей.
  Rows all_statistics();

  // Возвращает уведомления без ACK; account_id — ID адресата.
  Rows pending_notifications(std::int64_t account_id);

  // Подтверждает уведомление адресата; повторный ACK безопасен.
  // account_id — ID адресата; notification_id — ID сообщения.
  void acknowledge_notification(std::int64_t account_id,
                                std::int64_t notification_id);

 private:
  // Единственное соединение SQLite, принадлежащее объекту.
  sqlite3* database_ = nullptr;
  // Защищает все операции с общим соединением от параллельного доступа.
  std::mutex mutex_;

  // Создаёт таблицы и применяет миграции, совместимые со старыми базами.
  void initialize_schema();

  // Считает доверие сюжета story_id на момент now (Unix-секунды).
  // Вызывается под mutex_; current_settings задаёт период полураспада.
  double story_trust(std::int64_t story_id, std::int64_t now,
                     const Settings& current_settings);

  // Считает статистику клиента; login — имя входа, вызов требует mutex_.
  Rows statistics_for_login(const std::string& login);
};

}  // namespace edition
