// =============================================================================
// Файл: subscription_manager.cpp
// Реализация модуля подписок. Персистентность в БД сохраняется при отключении,
// удаляется только явной командой UNSUB.
// =============================================================================
#include "subscription_manager.h"
#include <stdexcept>
#include <cstdio>
#include <cstring>

#ifdef USE_SQLITE
#include <sqlite3.h>
#endif

SubscriptionManager::SubscriptionManager(sqlite3* db)
    : db_(db)
{
    if (pthread_rwlock_init(&rwlock_, nullptr) != 0) {
        throw std::runtime_error("Failed to initialize rwlock");
    }
}

SubscriptionManager::~SubscriptionManager() {
    pthread_rwlock_destroy(&rwlock_);
}

#ifdef USE_SQLITE
static bool exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "SQLite error: %s\n", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}
#endif

void SubscriptionManager::sub_set_client_id(int client_fd, const std::string& client_id) {
    fd_to_client_id_[client_fd] = client_id;
}

void SubscriptionManager::add_internal(const std::string& topic, int client_fd) {
    topic_to_subscribers_[topic].insert(client_fd);
    fd_to_topics_[client_fd].insert(topic);

#ifdef USE_SQLITE
    if (db_) {
        auto it = fd_to_client_id_.find(client_fd);
        if (it != fd_to_client_id_.end()) {
            const std::string& cid = it->second;
            char sql[512];
            std::snprintf(sql, sizeof(sql),
                         "INSERT OR IGNORE INTO subscriptions (client_id, topic) "
                         "VALUES ('%s', '%s');",
                         cid.c_str(), topic.c_str());
            exec_sql(db_, sql);
        }
    }
#endif
}

void SubscriptionManager::remove_internal(const std::string& topic, int client_fd) {
    auto ts_it = topic_to_subscribers_.find(topic);
    if (ts_it != topic_to_subscribers_.end()) {
        ts_it->second.erase(client_fd);
        if (ts_it->second.empty()) {
            topic_to_subscribers_.erase(ts_it);
        }
    }

    auto ft_it = fd_to_topics_.find(client_fd);
    if (ft_it != fd_to_topics_.end()) {
        ft_it->second.erase(topic);
        if (ft_it->second.empty()) {
            fd_to_topics_.erase(ft_it);
        }
    }

#ifdef USE_SQLITE
    if (db_) {
        auto id_it = fd_to_client_id_.find(client_fd);
        if (id_it != fd_to_client_id_.end()) {
            const std::string& cid = id_it->second;
            char sql[512];
            std::snprintf(sql, sizeof(sql),
                         "DELETE FROM subscriptions WHERE client_id = '%s' AND topic = '%s';",
                         cid.c_str(), topic.c_str());
            exec_sql(db_, sql);
        }
    }
#endif
}

void SubscriptionManager::remove_memory_only(const std::string& topic, int client_fd) {
    auto ts_it = topic_to_subscribers_.find(topic);
    if (ts_it != topic_to_subscribers_.end()) {
        ts_it->second.erase(client_fd);
        if (ts_it->second.empty()) {
            topic_to_subscribers_.erase(ts_it);
        }
    }

    auto ft_it = fd_to_topics_.find(client_fd);
    if (ft_it != fd_to_topics_.end()) {
        ft_it->second.erase(topic);
        if (ft_it->second.empty()) {
            fd_to_topics_.erase(ft_it);
        }
    }
    // НЕ трогаем БД при отключении клиента
}

bool SubscriptionManager::sub_add(const std::string& topic, int client_fd) {
    pthread_rwlock_wrlock(&rwlock_);
    auto& subscribers = topic_to_subscribers_[topic];
    bool exists = subscribers.find(client_fd) != subscribers.end();
    if (!exists) {
        add_internal(topic, client_fd);
    }
    pthread_rwlock_unlock(&rwlock_);
    return !exists;
}

bool SubscriptionManager::sub_remove(const std::string& topic, int client_fd) {
    pthread_rwlock_wrlock(&rwlock_);
    bool existed = false;
    auto ts_it = topic_to_subscribers_.find(topic);
    if (ts_it != topic_to_subscribers_.end() &&
        ts_it->second.find(client_fd) != ts_it->second.end())
    {
        existed = true;
        remove_internal(topic, client_fd);
    }
    pthread_rwlock_unlock(&rwlock_);
    return existed;
}

void SubscriptionManager::sub_remove_all(int client_fd) {
    pthread_rwlock_wrlock(&rwlock_);
    auto ft_it = fd_to_topics_.find(client_fd);
    if (ft_it != fd_to_topics_.end()) {
        auto topics_copy = ft_it->second;
        for (const auto& topic : topics_copy) {
            remove_memory_only(topic, client_fd);
        }
    }
    fd_to_client_id_.erase(client_fd);
    pthread_rwlock_unlock(&rwlock_);
}

std::vector<int> SubscriptionManager::sub_get_subscribers(const std::string& topic) const {
    pthread_rwlock_rdlock(&rwlock_);
    std::vector<int> result;
    auto it = topic_to_subscribers_.find(topic);
    if (it != topic_to_subscribers_.end()) {
        result.assign(it->second.begin(), it->second.end());
    }
    pthread_rwlock_unlock(&rwlock_);
    return result;
}

std::vector<std::string> SubscriptionManager::sub_get_topics(int client_fd) const {
    pthread_rwlock_rdlock(&rwlock_);
    std::vector<std::string> result;
    auto it = fd_to_topics_.find(client_fd);
    if (it != fd_to_topics_.end()) {
        result.assign(it->second.begin(), it->second.end());
    }
    pthread_rwlock_unlock(&rwlock_);
    return result;
}

bool SubscriptionManager::sub_restore(const std::string& client_id, int client_fd) {
#ifdef USE_SQLITE
    if (!db_) return false;

    pthread_rwlock_wrlock(&rwlock_);
    fd_to_client_id_[client_fd] = client_id;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT topic FROM subscriptions WHERE client_id = ?;";
    bool ok = true;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "SQLite prepare error: %s\n", sqlite3_errmsg(db_));
        ok = false;
    } else {
        sqlite3_bind_text(stmt, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);
        int restored_count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* topic = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (topic) {
                auto& subs = topic_to_subscribers_[topic];
                if (subs.find(client_fd) == subs.end()) {
                    add_internal(topic, client_fd);
                    restored_count++;
                }
            }
        }
        if (restored_count > 0) {
            std::fprintf(stderr, "[DB] Восстановлено %d подписок для %s\n",
                        restored_count, client_id.c_str());
        }
        sqlite3_finalize(stmt);
    }
    pthread_rwlock_unlock(&rwlock_);
    return ok;
#else
    (void)client_id;
    (void)client_fd;
    return false;
#endif
}