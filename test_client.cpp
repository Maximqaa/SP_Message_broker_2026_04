// =============================================================================
// Файл: test_client.cpp
// Пакетный клиент для автотестов.
// =============================================================================
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <vector>

static bool send_line(int fd, const std::string& line) {
    std::string data = line + "\n";
    ssize_t sent = send(fd, data.c_str(), data.size(), 0);
    return sent == static_cast<ssize_t>(data.size());
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <ip> <port> [commands...]\n";
        return 1;
    }

    const char* ip = argv[1];
    int port = std::atoi(argv[2]);
    std::vector<std::string> commands;
    for (int i = 3; i < argc; ++i) commands.push_back(argv[i]);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        std::cerr << "Invalid IP\n";
        close(sock);
        return 1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    if (!commands.empty()) {
        for (const auto& cmd : commands) send_line(sock, cmd);
        shutdown(sock, SHUT_WR);
        char buf[4096];
        while (true) {
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n > 0) std::cout.write(buf, n);
            else break;
        }
        close(sock);
        return 0;
    }

    // Интерактивный режим
    std::cout << "Connected. Type commands, Ctrl+D to exit.\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty()) send_line(sock, line);
    }
    close(sock);
    return 0;
}