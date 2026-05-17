// =============================================================================
// Файл: stats.h
// Потокобезопасная статистика брокера.
// =============================================================================
#ifndef STATS_H
#define STATS_H

#include <atomic>
#include <string>

#include <sstream>
#include <iomanip>

struct BrokerStats {
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> total_messages{0};
    std::atomic<uint64_t> total_subscriptions{0};
    std::atomic<uint64_t> total_unsubscriptions{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> errors{0};

    std::string to_string() const {
        std::ostringstream oss;
        oss << "Статистика брокера:\n"
            << "  Подключений всего: " << total_connections.load() << "\n"
            << "  Активных клиентов: " << active_connections.load() << "\n"
            << "  Сообщений доставлено: " << total_messages.load() << "\n"
            << "  Подписок создано: " << total_subscriptions.load() << "\n"
            << "  Отписок выполнено: " << total_unsubscriptions.load() << "\n"
            << "  Принято байт: " << format_bytes(bytes_received.load()) << "\n"
            << "  Отправлено байт: " << format_bytes(bytes_sent.load()) << "\n"
            << "  Ошибок: " << errors.load();
        return oss.str();
    }

private:
    static std::string format_bytes(uint64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024 && unit < 3) {
            size /= 1024;
            unit++;
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
        return oss.str();
    }
};

extern BrokerStats g_stats;

#endif
