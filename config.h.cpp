// =============================================================================
// Файл: config.h
// Конфигурация сервера (аргументы командной строки + переменные окружения).
// =============================================================================
#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include "logger.h"

struct ServerConfig {
    int port = 9000;
    std::string db_path;
    int max_clients = 1024;
    int buffer_size = 4096;
    int epoll_timeout_ms = 100;
    LogLevel log_level = LogLevel::INFO;
    bool daemonize = false;

    static ServerConfig from_args(int argc, char* argv[]) {
        ServerConfig cfg;

        const char* env_port = std::getenv("BROKER_PORT");
        if (env_port) cfg.port = std::atoi(env_port);

        const char* env_db = std::getenv("BROKER_DB");
        if (env_db) cfg.db_path = env_db;

        const char* env_debug = std::getenv("BROKER_DEBUG");
        if (env_debug && std::string(env_debug) == "1") {
            cfg.log_level = LogLevel::DEBUG;
        }

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-p" && i + 1 < argc) {
                cfg.port = std::atoi(argv[++i]);
            } else if (arg == "-d" && i + 1 < argc) {
                cfg.db_path = argv[++i];
            } else if (arg == "--debug") {
                cfg.log_level = LogLevel::DEBUG;
            } else if (arg == "--max-clients" && i + 1 < argc) {
                cfg.max_clients = std::atoi(argv[++i]);
            } else if (arg == "--daemon") {
                cfg.daemonize = true;
            } else if (arg == "-h" || arg == "--help") {
                print_usage(argv[0]);
                std::exit(0);
            } else {
                throw std::runtime_error("Unknown argument: " + arg);
            }
        }

        validate(cfg);
        return cfg;
    }

    static void print_usage(const char* prog) {
        std::cout << "Брокер сообщений v1.0\n\n"
                  << "Использование: " << prog << " [опции]\n\n"
                  << "Опции:\n"
                  << "  -p <порт>          Порт (по умолчанию: 9000)\n"
                  << "  -d <файл>          Файл БД SQLite для персистентности\n"
                  << "  --max-clients <N>  Макс. количество клиентов\n"
                  << "  --debug            Отладочное логирование\n"
                  << "  --daemon           Запустить как демон\n"
                  << "  -h, --help         Справка\n\n"
                  << "Переменные окружения:\n"
                  << "  BROKER_PORT, BROKER_DB, BROKER_DEBUG=1\n"
                  << std::endl;
    }

    static void validate(const ServerConfig& cfg) {
        if (cfg.port <= 0 || cfg.port > 65535)
            throw std::runtime_error("Invalid port number");
        if (cfg.max_clients < 1 || cfg.max_clients > 65535)
            throw std::runtime_error("Invalid max_clients value");
    }
};

#endif