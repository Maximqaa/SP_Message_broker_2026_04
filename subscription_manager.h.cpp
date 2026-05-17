// =============================================================================
// Файл: subscription_manager.h
// Потокобезопасный модуль управления подписками.
// =============================================================================
#ifndef SUBSCRIPTION_MANAGER_H
#define SUBSCRIPTION_MANAGER_H

#include <string>
#include <vector>
#include <unordered_map>

#include <unordered_set>
#include <pthread.h>

struct sqlite3;

class SubscriptionManager {
public:
    explicit SubscriptionManager(sqlite3* db = nullptr);
    ~SubscriptionManager();

    bool sub_add(const std::string& topic, int client_fd);
    bool sub_remove(const std::string& topic, int client_fd);
    void sub_remove_all(int client_fd);
    std::vector<int> sub_get_subscribers(const std::string& topic) const;
    std::vector<std::string> sub_get_topics(int client_fd) const;

    void sub_set_client_id(int client_fd, const std::string& client_id);
    bool sub_restore(const std::string& client_id, int client_fd);

private:
    void add_internal(const std::string& topic, int client_fd);
    void remove_internal(const std::string& topic, int client_fd);
    void remove_memory_only(const std::string& topic, int client_fd);

    std::unordered_map<std::string, std::unordered_set<int>> topic_to_subscribers_;
    std::unordered_map<int, std::unordered_set<std::string>> fd_to_topics_;
    std::unordered_map<int, std::string> fd_to_client_id_;

    mutable pthread_rwlock_t rwlock_;
    sqlite3* db_;
};

#endif
