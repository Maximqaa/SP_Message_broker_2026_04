// =============================================================================
// Файл: server_enhanced.cpp
// TCP-брокер сообщений. Расширенный формат сообщений + обратная совместимость.
// =============================================================================
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>

#ifdef USE_SQLITE
#include <sqlite3.h>
#endif

#include "subscription_manager.h"
#include "logger.h"
#include "config.h"
#include "stats.h"

constexpr int MAX_EVENTS = 1024;

volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_show_stats = 0;

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_running = 0;
        LOG_INFO("Получен сигнал завершения");
    } else if (signum == SIGUSR1) {
        g_show_stats = 1;
    }
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

struct Client {
    int fd = -1;
    std::string in_buffer;
    std::string out_buffer;
    std::string client_id;
    bool close_after_write = false;
    bool extended_mode = false;
};

static std::vector<std::string> split_command(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) tokens.push_back(token);
    return tokens;
}

static bool epoll_add(int epfd, int fd, uint32_t events) {
    epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

static bool epoll_mod(int epfd, int fd, uint32_t events) {
    epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

static void close_client(int epfd, Client& client, SubscriptionManager& subs) {
    if (client.fd >= 0) {
        LOG_DEBUG("Отключение fd=" + std::to_string(client.fd) + " id=" + client.client_id);
        epoll_ctl(epfd, EPOLL_CTL_DEL, client.fd, nullptr);
        shutdown(client.fd, SHUT_RDWR);
        close(client.fd);
        subs.sub_remove_all(client.fd);
        g_stats.active_connections--;
        client.fd = -1;
    }
}

int main(int argc, char* argv[]) {
    try {
        ServerConfig config = ServerConfig::from_args(argc, argv);
        Logger::instance().set_level(config.log_level);

        LOG_INFO("Запуск брокера сообщений v1.0");
        LOG_DEBUG("Порт: " + std::to_string(config.port));
        LOG_DEBUG("Макс. клиентов: " + std::to_string(config.max_clients));

#ifdef USE_SQLITE
        if (!config.db_path.empty())
            LOG_INFO("Персистентность: включена (" + config.db_path + ")");
        else
            LOG_INFO("Персистентность: выключена");
#else
        LOG_INFO("Персистентность: отключена (сборка без SQLite)");
#endif

        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGUSR1, &sa, nullptr);
        signal(SIGPIPE, SIG_IGN);

        sqlite3* db = nullptr;
#ifdef USE_SQLITE
        if (!config.db_path.empty()) {
            if (sqlite3_open(config.db_path.c_str(), &db) != SQLITE_OK) {
                LOG_ERROR("Не удалось открыть БД: " + std::string(sqlite3_errmsg(db)));
                return 1;
            }
            const char* create_sql =
                "CREATE TABLE IF NOT EXISTS subscriptions ("
                "client_id TEXT NOT NULL, "
                "topic TEXT NOT NULL, "
                "PRIMARY KEY (client_id, topic));";
            char* err_msg = nullptr;
            if (sqlite3_exec(db, create_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
                LOG_ERROR("Ошибка создания таблицы: " + std::string(err_msg));
                sqlite3_free(err_msg);
                return 1;
            }
        }
#endif

        SubscriptionManager sub_mgr(db);

        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            LOG_ERROR("Ошибка socket()");
            return 1;
        }

        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listen_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(config.port));

        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            LOG_ERROR("Ошибка bind()");
            close(listen_fd);
            return 1;
        }

        if (listen(listen_fd, SOMAXCONN) < 0) {
            LOG_ERROR("Ошибка listen()");
            close(listen_fd);
            return 1;
        }

        set_nonblocking(listen_fd);

        int epfd = epoll_create1(0);
        if (epfd < 0) {
            LOG_ERROR("Ошибка epoll_create1()");
            close(listen_fd);
            return 1;
        }

        epoll_add(epfd, listen_fd, EPOLLIN);

        std::unordered_map<int, Client> clients;
        char read_buf[4096];

        LOG_INFO("Брокер слушает порт " + std::to_string(config.port));

        epoll_event events[MAX_EVENTS];

        while (g_running) {
            int nfds = epoll_wait(epfd, events, MAX_EVENTS, config.epoll_timeout_ms);
            if (nfds < 0) {
                if (errno == EINTR) {
                    if (g_show_stats) {
                        g_show_stats = 0;
                        LOG_INFO("\n" + g_stats.to_string());
                    }
                    continue;
                }
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;

                if (fd == listen_fd) {
                    while (true) {
                        sockaddr_in client_addr;
                        socklen_t addrlen = sizeof(client_addr);
                        int cfd = accept(listen_fd,
                                        reinterpret_cast<sockaddr*>(&client_addr),
                                        &addrlen);
                        if (cfd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            break;
                        }

                        uint64_t current = g_stats.active_connections.load();
                        if (static_cast<int>(current) >= config.max_clients) {
                            LOG_WARN("Превышен лимит подключений");
                            close(cfd);
                            continue;
                        }

                        set_nonblocking(cfd);

                        Client client;
                        client.fd = cfd;
                        client.extended_mode = false;

                        static std::atomic<int> id_counter{0};
                        std::ostringstream id_oss;
                        id_oss << "c_" << std::setw(4) << std::setfill('0') << ++id_counter;
                        std::string new_id = id_oss.str();
                        client.client_id = new_id;
                        sub_mgr.sub_set_client_id(cfd, new_id);

                        char client_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &client_addr.sin_addr,
                                 client_ip, sizeof(client_ip));
                        LOG_INFO("Подключение: fd=" + std::to_string(cfd) +
                                " id=" + new_id + " ip=" + client_ip);

                        client.out_buffer = "+OK Брокер v1.0. Ваш ID: " + new_id + "\n";
                        client.out_buffer += "+OK Для красивого формата отправьте: MODE EXTENDED\n";

                        epoll_add(epfd, cfd, EPOLLIN | EPOLLOUT);
                        g_stats.total_connections++;
                        g_stats.active_connections++;
                        clients.emplace(cfd, std::move(client));
                    }
                }
                else {
                    auto it = clients.find(fd);
                    if (it == clients.end()) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        continue;
                    }

                    Client& client = it->second;

                    if ((ev & (EPOLLERR | EPOLLHUP)) && !(ev & EPOLLIN)) {
                        close_client(epfd, client, sub_mgr);
                        clients.erase(it);
                        continue;
                    }

                    if (ev & EPOLLIN) {
                        while (true) {
                            ssize_t n = read(fd, read_buf, sizeof(read_buf));
                            if (n > 0) {
                                g_stats.bytes_received += n;
                                client.in_buffer.append(read_buf, n);
                            } else if (n == 0) {
                                close_client(epfd, client, sub_mgr);
                                clients.erase(it);
                                goto next_client;
                            } else {
                                if (errno == EAGAIN) break;
                                close_client(epfd, client, sub_mgr);
                                clients.erase(it);
                                goto next_client;
                            }
                        }

                        while (true) {
                            size_t pos = client.in_buffer.find('\n');
                            if (pos == std::string::npos) break;
                            std::string line = client.in_buffer.substr(0, pos);
                            client.in_buffer.erase(0, pos + 1);
                            if (line.empty()) continue;
                            if (line.back() == '\r') line.pop_back();

                            auto tokens = split_command(line);
                            if (tokens.empty()) continue;
                            std::string cmd = tokens[0];

                            if (cmd == "SUB" && tokens.size() >= 2) {
                                if (sub_mgr.sub_add(tokens[1], fd)) {
                                    client.out_buffer += "+OK Подписка на '" + tokens[1] + "'\n";
                                    g_stats.total_subscriptions++;
                                } else {
                                    client.out_buffer += "+OK Уже подписаны на '" + tokens[1] + "'\n";
                                }
                            }
                            else if (cmd == "UNSUB" && tokens.size() >= 2) {
                                if (sub_mgr.sub_remove(tokens[1], fd)) {
                                    client.out_buffer += "+OK Отписка от '" + tokens[1] + "'\n";
                                    g_stats.total_unsubscriptions++;
                                } else {
                                    client.out_buffer += "-ERR Не подписаны на '" + tokens[1] + "'\n";
                                }
                            }
                            else if (cmd == "PUB" && tokens.size() >= 3) {
                                std::string topic = tokens[1];
                                size_t msg_start = line.find(topic, cmd.size()) + topic.size();
                                while (msg_start < line.size() && line[msg_start] == ' ') msg_start++;
                                std::string msg = line.substr(msg_start);

                                // Получаем текущее время
                                auto now = std::chrono::system_clock::now();
                                std::time_t now_c = std::chrono::system_clock::to_time_t(now);
                                std::string timestamp = std::ctime(&now_c);
                                if (!timestamp.empty()) timestamp.pop_back();

                                std::string sender_id = client.client_id.empty() ? "unknown" : client.client_id;

                                auto subscribers = sub_mgr.sub_get_subscribers(topic);
                                int delivered = 0;
                                for (int sfd : subscribers) {
                                    if (sfd == fd) continue;
                                    auto sub_it = clients.find(sfd);
                                    if (sub_it != clients.end()) {
                                        Client& sub_client = sub_it->second;
                                        size_t msg_bytes = 0;
                                        if (sub_client.extended_mode) {
                                            std::ostringstream oss;
                                            oss << "MESSAGE\n"
                                                << "Topic: " << topic << "\n"
                                                << "Timestamp: " << timestamp << "\n"
                                                << "Sender: " << sender_id << "\n"
                                                << "Payload: " << msg << "\n"
                                                << "ENDMESSAGE\n";
                                            std::string formatted = oss.str();
                                            msg_bytes = formatted.size();
                                            sub_client.out_buffer += formatted;
                                        } else {
                                            std::string legacy = "MESSAGE " + topic + " " + msg + "\n";
                                            msg_bytes = legacy.size();
                                            sub_client.out_buffer += legacy;
                                        }
                                        epoll_mod(epfd, sfd, EPOLLIN | EPOLLOUT);
                                        delivered++;
                                        g_stats.total_messages++;
                                        g_stats.bytes_sent += msg_bytes;
                                    }
                                }
                                client.out_buffer += "+OK Доставлено " + std::to_string(delivered) +
                                                   " подписчикам '" + topic + "'\n";
                            }
                            else if (cmd == "MODE" && tokens.size() >= 2 && tokens[1] == "EXTENDED") {
                                client.extended_mode = true;
                                client.out_buffer += "+OK Расширенный формат сообщений включён\n";
                            }
                            else if (cmd == "QUIT") {
                                client.out_buffer += "+OK До свидания\n";
                                client.close_after_write = true;
                                epoll_mod(epfd, fd, EPOLLOUT);
                            }
                            else if (cmd == "PING") {
                                client.out_buffer += "+PONG\n";
                            }
                            else if (cmd == "STATS") {
                                client.out_buffer += "+OK " + g_stats.to_string() + "\n.\n";
                            }
                            else if (cmd == "RECONNECT" && tokens.size() >= 2) {
#ifdef USE_SQLITE
                                if (db) {
                                    std::string req_id = tokens[1];
                                    client.client_id = req_id;
                                    sub_mgr.sub_set_client_id(fd, req_id);
                                    if (sub_mgr.sub_restore(req_id, fd)) {
                                        auto restored = sub_mgr.sub_get_topics(fd);
                                        client.out_buffer += "+OK Подписки восстановлены. ID: " +
                                                           req_id + "\n";
                                        if (!restored.empty()) {
                                            client.out_buffer += "+OK Восстановлены: ";
                                            for (size_t j = 0; j < restored.size(); ++j) {
                                                if (j > 0) client.out_buffer += ", ";
                                                client.out_buffer += restored[j];
                                            }
                                            client.out_buffer += "\n";
                                        }
                                    } else {
                                        client.out_buffer += "-ERR Ошибка восстановления\n";
                                    }
                                } else
#endif
                                {
                                    client.out_buffer += "-ERR Персистентность недоступна\n";
                                }
                            }
                            else {
                                client.out_buffer += "-ERR Неизвестная команда\n";
                            }
                        }

                        if (!client.out_buffer.empty() && !client.close_after_write)
                            epoll_mod(epfd, fd, EPOLLIN | EPOLLOUT);
                    }

                    if (ev & EPOLLOUT) {
                        if (!client.out_buffer.empty()) {
                            ssize_t n = write(fd, client.out_buffer.data(),
                                           client.out_buffer.size());
                            if (n > 0) {
                                g_stats.bytes_sent += n;
                                client.out_buffer.erase(0, n);
                            } else if (n < 0 && errno != EAGAIN) {
                                close_client(epfd, client, sub_mgr);
                                clients.erase(it);
                                goto next_client;
                            }

                            if (client.out_buffer.empty() && client.close_after_write) {
                                close_client(epfd, client, sub_mgr);
                                clients.erase(it);
                                goto next_client;
                            }

                            if (client.out_buffer.empty())
                                epoll_mod(epfd, fd, EPOLLIN);
                        }
                    }
                    next_client:;
                }
            }

            if (g_show_stats) {
                g_show_stats = 0;
                LOG_INFO("\n" + g_stats.to_string());
            }
        }

        LOG_INFO("Завершение работы...");
        for (auto& pair : clients)
            close_client(epfd, pair.second, sub_mgr);
        clients.clear();

        close(listen_fd);
        close(epfd);

#ifdef USE_SQLITE
        if (db) sqlite3_close(db);
#endif

        LOG_INFO("Брокер остановлен.\n" + g_stats.to_string());
        return 0;
    }
    catch (const std::exception& e) {
        LOG_ERROR(std::string("Критическая ошибка: ") + e.what());
        return 1;
    }
}