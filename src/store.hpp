#pragma once

#include "common.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

struct sqlite3;

namespace edition {

struct Account {
    std::int64_t id = 0;
    std::string login;
    std::string role;
    double trust = 0.0;
};

struct Submission {
    std::string status;
    std::string reason;
    std::int64_t story_id = 0;
    double trust = 0.0;
    bool ready = false;
    bool replayed = false;
};

struct Settings {
    double threshold = 1.01;
    std::int64_t half_life_seconds = 21600;
    std::int64_t congratulations_count = 3;
};

class Store {
public:
    // Persistent SQLite state for accounts, stories, votes and notifications.
    explicit Store(const std::string& path);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void seed_initial_clients();
    bool has_active_editor();
    bool account_is_active(std::int64_t account_id);
    std::int64_t last_received_order();
    Account authenticate(const std::string& login,
                         const std::string& password);
    Account add_account(const std::string& role, const std::string& login,
                        double trust);
    void rename_account(const std::string& old_login,
                        const std::string& new_login);
    void set_account_trust(const std::string& login, double trust);
    void set_account_password(const std::string& login,
                              const std::string& password);
    void set_own_password(std::int64_t account_id,
                          const std::string& password);
    void delete_account(const std::string& login,
                        std::int64_t current_account_id);
    void enable_account(const std::string& login);
    Rows list_accounts();

    Settings settings();
    void set_setting(const std::string& name, double value);

    Submission submit_note(std::int64_t account_id,
                           const std::string& request_id,
                           const std::string& category,
                           const std::string& topic,
                           std::int64_t event_time,
                           std::int64_t received_at,
                           std::int64_t received_order);
    Rows list_stories();
    Rows story_details(std::int64_t story_id);
    Rows publish_story(std::int64_t story_id);
    Rows statistics(std::int64_t account_id);
    Rows statistics_for_user(const std::string& login);
    Rows all_statistics();
    Rows pending_notifications(std::int64_t account_id);
    void acknowledge_notification(std::int64_t account_id,
                                  std::int64_t notification_id);

private:
    sqlite3* database_ = nullptr;
    std::mutex mutex_;

    void initialize_schema();
    double story_trust(std::int64_t story_id, std::int64_t now,
                       const Settings& current_settings);
    Rows statistics_for_login(const std::string& login);
};

}  // namespace edition
