// =============================================================================
// Файл: interactive_client.cpp
// Клиент с цветным интерфейсом, отображением подписок и расширенным форматом.
// =============================================================================
#include <iostream>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <vector>
#include <set>
#include <algorithm>

namespace Color {
    const char* RESET   = "\033[0m";
    const char* GREEN   = "\033[32m";
    const char* RED     = "\033[31m";
    const char* YELLOW  = "\033[33m";
    const char* CYAN    = "\033[36m";
    const char* MAGENTA = "\033[35m";
    const char* BOLD    = "\033[1m";
    const char* DIM     = "\033[2m";
}

static std::set<std::string> my_topics;
static bool use_colors = true;
static bool show_timestamp = true;
static bool compact_mode = false;

static bool send_line(int fd, const std::string& line) {
    std::string data = line + "\n";
    ssize_t sent = send(fd, data.c_str(), data.size(), 0);
    return sent == static_cast<ssize_t>(data.size());
}

static void show_current_topics() {
    if (my_topics.empty()) {
        std::cout << (use_colors ? Color::DIM : "")
                  << "Нет активных подписок. Используйте SUB <topic> или s <topic>"
                  << (use_colors ? Color::RESET : "") << std::endl;
    } else {
        std::cout << (use_colors ? Color::CYAN : "")
                  << "Активные подписки: "
                  << (use_colors ? Color::YELLOW : "");
        bool first = true;
        for (const auto& t : my_topics) {
            if (!first) std::cout << ", ";
            std::cout << t;
            first = false;
        }
        std::cout << (use_colors ? Color::RESET : "") << std::endl;
    }
}

static void print_prompt() {
    if (!use_colors) {
        if (!my_topics.empty()) {
            std::cout << "[";
            bool first = true;
            for (const auto& t : my_topics) {
                if (!first) std::cout << ",";
                std::cout << t;
                first = false;
            }
            std::cout << "]";
        }
        std::cout << "> " << std::flush;
        return;
    }
    std::cout << Color::CYAN;
    if (!my_topics.empty()) {
        std::cout << "[";
        bool first = true;
        for (const auto& t : my_topics) {
            if (!first) std::cout << ",";
            std::cout << t;
            first = false;
        }
        std::cout << "]";
    }
    std::cout << "> " << Color::RESET << std::flush;
}

// Обновляет множество подписок на основе строки ответа сервера
// Возвращает true, если множество изменилось
static bool update_subscriptions_from_line(const std::string& line) {
    bool changed = false;
    if (line.find("+OK Подписка на '") != std::string::npos) {
        size_t s = line.find("'") + 1;
        size_t e = line.find("'", s);
        if (e != std::string::npos) {
            my_topics.insert(line.substr(s, e - s));
            changed = true;
        }
    }
    else if (line.find("+OK Отписка от '") != std::string::npos) {
        size_t s = line.find("'") + 1;
        size_t e = line.find("'", s);
        if (e != std::string::npos) {
            my_topics.erase(line.substr(s, e - s));
            changed = true;
        }
    }
    else if (line.find("+OK Восстановлены:") != std::string::npos) {
        size_t s = line.find(":") + 2;
        std::string topics = line.substr(s);
        std::istringstream tss(topics);
        std::string t;
        my_topics.clear();
        while (std::getline(tss, t, ',')) {
            if (!t.empty() && t[0] == ' ') t.erase(0, 1);
            if (!t.empty()) my_topics.insert(t);
        }
        changed = true;
    }
    else if (line.find("+OK Подписки восстановлены") != std::string::npos) {
        my_topics.clear();
        changed = true;
    }
    return changed;
}

static void display_extended_message(const std::string& block) {
    std::istringstream iss(block);
    std::string line;
    std::string topic, timestamp, sender, payload;

    while (std::getline(iss, line)) {
        if (line.find("Topic:") == 0) topic = line.substr(6);
        else if (line.find("Timestamp:") == 0) timestamp = line.substr(10);
        else if (line.find("Sender:") == 0) sender = line.substr(7);
        else if (line.find("Payload:") == 0) payload = line.substr(8);
        else if (line == "ENDMESSAGE") break;
    }

    if (!use_colors) {
        if (show_timestamp)
            std::cout << "[MSG] " << topic << " from " << sender << " at " << timestamp << ": " << payload << std::endl;
        else
            std::cout << "[MSG] " << topic << " from " << sender << ": " << payload << std::endl;
        std::cout << std::endl;
        return;
    }

    if (compact_mode) {
        std::cout << Color::MAGENTA << "[" << topic << "] " << Color::RESET << payload;
        if (show_timestamp && !timestamp.empty())
            std::cout << Color::DIM << " (" << timestamp << ")" << Color::RESET;
        std::cout << std::endl << std::endl;
        return;
    }

    std::cout << "\n";
    std::cout << Color::CYAN << "┌─────────────────────────────────────────────┐" << Color::RESET << "\n";
    std::cout << "│ " << Color::YELLOW << "📨 " << topic << Color::RESET;
    if (show_timestamp && !timestamp.empty()) {
        std::cout << " @ " << Color::DIM << timestamp << Color::RESET;
    }
    std::cout << "\n";
    std::cout << "│ " << Color::MAGENTA << "From: " << sender << Color::RESET << "\n";
    std::cout << Color::CYAN << "├─────────────────────────────────────────────┤" << Color::RESET << "\n";

    size_t start = 0;
    while (start < payload.length()) {
        size_t len = std::min<size_t>(40, payload.length() - start);
        std::cout << "│ " << payload.substr(start, len);
        if (len < 40) std::cout << std::string(40 - len, ' ');
        std::cout << " │\n";
        start += len;
    }
    std::cout << Color::CYAN << "└─────────────────────────────────────────────┘" << Color::RESET << std::endl;
    std::cout << std::endl;
}

// Обрабатывает полученные данные, извлекает обычные строки и блоки MESSAGE.
// Возвращает необработанный остаток.
static std::string process_server_response(const std::string& data) {
    std::string remaining;
    size_t pos = 0;
    while (pos < data.size()) {
        if (data.substr(pos, 7) == "MESSAGE") {
            size_t end = data.find("ENDMESSAGE", pos);
            if (end != std::string::npos) {
                end += 10;
                std::string block = data.substr(pos, end - pos);
                display_extended_message(block);
                pos = end;
                continue;
            } else {
                remaining = data.substr(pos);
                break;
            }
        }
        size_t next_msg = data.find("MESSAGE", pos);
        if (next_msg == std::string::npos) {
            std::string plain = data.substr(pos);
            if (!plain.empty()) {
                std::istringstream iss(plain);
                std::string line;
                bool subs_changed = false;
                while (std::getline(iss, line)) {
                    if (line.empty()) continue;
                    // Обновляем подписки на основе строки
                    if (update_subscriptions_from_line(line))
                        subs_changed = true;
                    // Выводим строку с цветом
                    if (!use_colors) {
                        std::cout << line << std::endl;
                    } else {
                        if (line.rfind("+OK", 0) == 0)
                            std::cout << Color::GREEN << line << Color::RESET << std::endl;
                        else if (line.rfind("-ERR", 0) == 0)
                            std::cout << Color::RED << line << Color::RESET << std::endl;
                        else
                            std::cout << line << std::endl;
                    }
                }
                if (subs_changed && !compact_mode) {
                    show_current_topics();
                }
            }
            break;
        } else {
            std::string plain = data.substr(pos, next_msg - pos);
            if (!plain.empty()) {
                std::istringstream iss(plain);
                std::string line;
                bool subs_changed = false;
                while (std::getline(iss, line)) {
                    if (line.empty()) continue;
                    if (update_subscriptions_from_line(line))
                        subs_changed = true;
                    if (!use_colors) {
                        std::cout << line << std::endl;
                    } else {
                        if (line.rfind("+OK", 0) == 0)
                            std::cout << Color::GREEN << line << Color::RESET << std::endl;
                        else if (line.rfind("-ERR", 0) == 0)
                            std::cout << Color::RED << line << Color::RESET << std::endl;
                        else
                            std::cout << line << std::endl;
                    }
                }
                if (subs_changed && !compact_mode) {
                    show_current_topics();
                }
            }
            pos = next_msg;
        }
    }
    return remaining;
}

static void print_help() {
    if (!use_colors) {
        std::cout << "\nКоманды брокера:\n"
                  << "  SUB <topic>        - подписаться\n"
                  << "  UNSUB <topic>      - отписаться\n"
                  << "  PUB <topic> <msg>  - опубликовать\n"
                  << "  QUIT               - отключиться\n"
                  << "  PING               - проверка связи\n"
                  << "  STATS              - статистика\n"
                  << "  RECONNECT <id>     - восстановить подписки\n"
                  << "\nСокращения:\n"
                  << "  s <topic>   → SUB\n"
                  << "  u <topic>   → UNSUB\n"
                  << "  p <topic> <msg> → PUB\n"
                  << "  <текст> → PUB в первый топик\n"
                  << "\nУправление:\n"
                  << "  /timestamp   - показать/скрыть время\n"
                  << "  /compact     - компактный режим\n"
                  << "  /nocolor     - отключить цвета\n"
                  << "  /topics      - показать активные подписки\n"
                  << "  /clear       - очистить экран\n"
                  << "  /help        - эта справка\n"
                  << "  /quit        - выход\n\n";
        return;
    }
    std::cout << Color::BOLD << "\nКоманды брокера:\n" << Color::RESET
              << Color::GREEN << "  SUB <topic>" << Color::RESET
              << "        - подписаться\n"
              << Color::GREEN << "  UNSUB <topic>" << Color::RESET
              << "      - отписаться\n"
              << Color::GREEN << "  PUB <topic> <msg>" << Color::RESET
              << "  - опубликовать\n"
              << Color::GREEN << "  QUIT" << Color::RESET
              << "               - отключиться\n"
              << Color::GREEN << "  PING" << Color::RESET
              << "               - проверка связи\n"
              << Color::GREEN << "  STATS" << Color::RESET
              << "             - статистика\n"
              << Color::GREEN << "  RECONNECT <id>" << Color::RESET
              << "   - восстановить подписки\n"
              << "\nСокращения:\n"
              << Color::YELLOW << "  s <topic>" << Color::RESET
              << "   → SUB\n"
              << Color::YELLOW << "  u <topic>" << Color::RESET
              << "   → UNSUB\n"
              << Color::YELLOW << "  p <topic> <msg>" << Color::RESET
              << " → PUB\n"
              << Color::YELLOW << "  <текст>" << Color::RESET
              << "      → PUB в первый топик\n"
              << "\nУправление:\n"
              << Color::YELLOW << "  /timestamp" << Color::RESET
              << "   - показать/скрыть время\n"
              << Color::YELLOW << "  /compact" << Color::RESET
              << "     - компактный режим\n"
              << Color::YELLOW << "  /nocolor" << Color::RESET
              << "     - отключить цвета\n"
              << Color::YELLOW << "  /topics" << Color::RESET
              << "      - показать активные подписки\n"
              << Color::YELLOW << "  /clear" << Color::RESET
              << "       - очистить экран\n"
              << Color::YELLOW << "  /help" << Color::RESET
              << "        - эта справка\n"
              << Color::YELLOW << "  /quit" << Color::RESET
              << "        - выход\n\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Использование: " << argv[0] << " <ip> <порт> [--no-color]\n";
        return 1;
    }

    const char* ip = argv[1];
    int port = std::atoi(argv[2]);

    for (int i = 3; i < argc; i++)
        if (std::string(argv[i]) == "--no-color") use_colors = false;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        std::cerr << "Неверный IP\n";
        close(sock);
        return 1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << (use_colors ? Color::RED : "")
                  << "Ошибка подключения: " << strerror(errno)
                  << (use_colors ? Color::RESET : "") << "\n";
        close(sock);
        return 1;
    }

    send_line(sock, "MODE EXTENDED");

    std::cout << (use_colors ? Color::GREEN : "")
              << "Подключено к " << ip << ":" << port
              << (use_colors ? Color::RESET : "") << "\n";
    print_help();
    show_current_topics();

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = sock;
    fds[1].events = POLLIN;

    std::string incoming_buffer;

    while (true) {
        print_prompt();
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN) {
            std::string line;
            if (!std::getline(std::cin, line)) break;
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            if (line.empty()) continue;

            if (line[0] == '/') {
                std::string c = line.substr(1);
                if (c == "help") {
                    print_help();
                    continue;
                }
                else if (c == "topics") {
                    show_current_topics();
                    continue;
                }
                else if (c == "clear") {
                    std::cout << "\033[2J\033[1;1H";
                    continue;
                }
                else if (c == "nocolor") {
                    use_colors = !use_colors;
                    std::cout << "Цвета " << (use_colors ? "включены" : "отключены") << std::endl;
                    continue;
                }
                else if (c == "timestamp") {
                    show_timestamp = !show_timestamp;
                    std::cout << "Отображение времени " << (show_timestamp ? "включено" : "отключено") << std::endl;
                    continue;
                }
                else if (c == "compact") {
                    compact_mode = !compact_mode;
                    std::cout << "Компактный режим " << (compact_mode ? "включён" : "выключен") << std::endl;
                    continue;
                }
                else if (c == "quit" || c == "exit") {
                    send_line(sock, "QUIT");
                    usleep(100000);
                    break;
                }
                else {
                    std::cout << "Неизвестная команда. /help для списка." << std::endl;
                    continue;
                }
            }

            std::istringstream iss(line);
            std::string first;
            iss >> first;
            std::string cmd;

            if (first == "s" || first == "S") {
                std::string t;
                iss >> t;
                if (!t.empty()) cmd = "SUB " + t;
                else std::cout << "Укажите топик для подписки" << std::endl;
            }
            else if (first == "u" || first == "U") {
                std::string t;
                iss >> t;
                if (!t.empty()) cmd = "UNSUB " + t;
                else {
                    std::cout << "Укажите топик. Подписки: ";
                    for (auto& x : my_topics) std::cout << x << " ";
                    std::cout << std::endl;
                }
            }
            else if (first == "p" || first == "P") {
                std::string t, m;
                iss >> t;
                std::getline(iss >> std::ws, m);
                if (!t.empty() && !m.empty()) cmd = "PUB " + t + " " + m;
                else std::cout << "Формат: p <topic> <message>" << std::endl;
            }
            else if (first == "q" || first == "Q" || first == "QUIT") {
                cmd = "QUIT";
            }
            else if (first == "SUB" || first == "UNSUB" || first == "PUB" ||
                     first == "PING" || first == "STATS" || first == "RECONNECT") {
                cmd = line;
            }
            else {
                if (!my_topics.empty()) {
                    cmd = "PUB " + *my_topics.begin() + " " + line;
                } else {
                    std::string t = first;
                    std::string m;
                    std::getline(iss >> std::ws, m);
                    if (!t.empty() && !m.empty())
                        cmd = "PUB " + t + " " + m;
                    else
                        std::cout << "Нет подписок. Используйте s <topic> или PUB <topic> <msg>" << std::endl;
                }
            }

            if (!cmd.empty()) {
                if (!send_line(sock, cmd)) break;
                if (cmd == "QUIT") { usleep(100000); break; }
            }
        }

        if (fds[1].revents & POLLIN) {
            char buf[16384];
            ssize_t n = read(sock, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                incoming_buffer.append(buf, n);
                std::string remaining = process_server_response(incoming_buffer);
                incoming_buffer = remaining;
            } else if (n == 0) {
                std::cout << (use_colors ? Color::YELLOW : "")
                          << "\n[Сервер закрыл соединение]\n"
                          << (use_colors ? Color::RESET : "");
                break;
            } else {
                if (errno != EAGAIN) {
                    std::cout << (use_colors ? Color::RED : "")
                              << "\n[Ошибка чтения]\n"
                              << (use_colors ? Color::RESET : "");
                    break;
                }
            }
        }

        if (fds[1].revents & (POLLERR | POLLHUP)) {
            std::cout << (use_colors ? Color::RED : "")
                      << "\n[Соединение разорвано]\n"
                      << (use_colors ? Color::RESET : "");
            break;
        }
    }

    close(sock);
    return 0;
}