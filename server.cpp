#include "server.h"
#include <sys/epoll.h>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sstream>
#include "resp_parser.h"
#include <fcntl.h>
#include <errno.h>

void setNonBlocking(int fd);  // Forward declaration

TcpServer::TcpServer(int p) : server_fd(-1), port(p), running(false) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::start() {
    createSocket();
    bindSocket();
    listenSocket();

    running = true;
    std::cout << "Server listening on port " << port << "\n";

    eventLoop();
}

void TcpServer::stop() {
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    running = false;
}

void TcpServer::createSocket() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    setNonBlocking(server_fd);  // ✅ Make server socket non-blocking too!
}

void TcpServer::bindSocket() {
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        throw std::runtime_error("Bind failed");
    }
}

void TcpServer::listenSocket() {
    if (listen(server_fd, SOMAXCONN) < 0) {
        throw std::runtime_error("Listen failed");
    }
}


void setNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void TcpServer::eventLoop() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) throw std::runtime_error("epoll_create failed");

    epoll_event ev{}, events[1024];

    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    while (running) {
        int n = epoll_wait(epoll_fd, events, 1024, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // =========================
            // 🔥 NEW CONNECTION
            // =========================
            if (fd == server_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);

                    int client_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
                    if (client_fd < 0) break;

                    connected_clients++;

                    setNonBlocking(client_fd);

                    epoll_event cev{};
                    cev.events = EPOLLIN;
                    cev.data.fd = client_fd;

                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                }
            }

            // =========================
            // 🔥 CLIENT REQUEST
            // =========================
            else {
                char buffer[1024];

                ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);

                if (bytes < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // No data available right now, just skip
                        continue;
                    }
                    // Real error
                    close(fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                    connected_clients--;
                    continue;
                }

                if (bytes == 0) {
                    // Connection closed by client
                    close(fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                    connected_clients--;
                    continue;
                }

                std::string input(buffer, bytes);
                std::vector<std::string> tokens;

                try {
                    tokens = parseRESP(input);
                    total_commands++;
                } catch (...) {
                    std::string err = "-ERR invalid request\r\n";
                    send(fd, err.c_str(), err.size(), 0);
                    continue;
                }

                if (tokens.empty()) continue;

                std::string command = tokens[0];
                std::string response;

                try {

                    // =========================
                    // 🔥 STRING
                    // =========================
                    if (command == "SET") {
                        if (tokens.size() < 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            std::optional<int> ttl;

                            if (tokens.size() == 5 && tokens[3] == "EX") {
                                ttl = std::stoi(tokens[4]);
                            }

                            store.set(tokens[1], tokens[2], ttl);
                            response = "+OK\r\n";
                        }
                    }

                    // 🔥 GET
                    else if (command == "GET") {
                        if (tokens.size() < 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            auto val = store.get(tokens[1]);

                            if (val) {
                                response = "$" + std::to_string(val->size()) + "\r\n" + *val + "\r\n";
                            } else {
                                response = "$-1\r\n";
                            }
                        }
                    }

                    // 🔥 DEL
                    else if (command == "DEL") {
                        int deleted = store.del(tokens[1]) ? 1 : 0;
                        response = ":" + std::to_string(deleted) + "\r\n";
                    }

                    // 🔥 EXISTS
                    else if (command == "EXISTS") {
                        int exists = store.exists(tokens[1]) ? 1 : 0;
                        response = ":" + std::to_string(exists) + "\r\n";
                    }

                    // 🔥 KEYS
                    else if (command == "KEYS") {
                        auto keys = store.keys();

                        response = "*" + std::to_string(keys.size()) + "\r\n";
                        for (auto& k : keys) {
                            response += "$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
                        }
                    }

                    else if (command == "LPUSH") {
                        if (tokens.size() < 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            std::string key = tokens[1];
                            std::vector<std::string> values(tokens.begin() + 2, tokens.end());

                            try {
                                int size = store.lpush(key, values);
                                response = ":" + std::to_string(size) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "LPOP") {
                        if (tokens.size() != 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                auto val = store.lpop(tokens[1]);

                                if (val) {
                                    response = "$" + std::to_string(val->size()) + "\r\n" + *val + "\r\n";
                                } else {
                                    response = "$-1\r\n";  // nil
                                }

                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "RPUSH") {
                        if (tokens.size() < 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            std::string key = tokens[1];
                            std::vector<std::string> values(tokens.begin() + 2, tokens.end());

                            try {
                                int size = store.rpush(key, values);
                                response = ":" + std::to_string(size) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "RPOP") {
                        if (tokens.size() != 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                auto val = store.rpop(tokens[1]);

                                if (val) {
                                    response = "$" + std::to_string(val->size()) + "\r\n" + *val + "\r\n";
                                } else {
                                    response = "$-1\r\n";
                                }

                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "LRANGE") {
                        if (tokens.size() != 4) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            std::string key = tokens[1];
                            int start = std::stoi(tokens[2]);
                            int end = std::stoi(tokens[3]);

                            try {
                                auto result = store.lrange(key, start, end);

                                response = "*" + std::to_string(result.size()) + "\r\n";

                                for (auto& val : result) {
                                    response += "$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
                                }

                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }

                    else if (command == "LLEN") {
                        if (tokens.size() != 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                int size = store.llen(tokens[1]);
                                response = ":" + std::to_string(size) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }

                    else if (command == "SADD") {
                        if (tokens.size() < 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            std::string key = tokens[1];
                            std::vector<std::string> values(tokens.begin() + 2, tokens.end());

                            try {
                                int added = store.sadd(key, values);
                                response = ":" + std::to_string(added) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "SREM") {
                        if (tokens.size() < 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            std::string key = tokens[1];
                            std::vector<std::string> values(tokens.begin() + 2, tokens.end());

                            try {
                                int removed = store.srem(key, values);
                                response = ":" + std::to_string(removed) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "SMEMBERS") {
                        if (tokens.size() != 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                auto result = store.smembers(tokens[1]);

                                response = "*" + std::to_string(result.size()) + "\r\n";

                                for (auto& v : result) {
                                    response += "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
                                }

                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "SISMEMBER") {
                        if (tokens.size() != 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                bool exists = store.sismember(tokens[1], tokens[2]);
                                response = ":" + std::to_string(exists ? 1 : 0) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "SCARD") {
                        if (tokens.size() != 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                int size = store.scard(tokens[1]);
                                response = ":" + std::to_string(size) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "HSET") {
                        if (tokens.size() != 4) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                int res = store.hset(tokens[1], tokens[2], tokens[3]);
                                response = ":" + std::to_string(res) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }

                    else if (command == "HGET") {
                        if (tokens.size() != 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                auto val = store.hget(tokens[1], tokens[2]);

                                if (val) {
                                    response = "$" + std::to_string(val->size()) + "\r\n" + *val + "\r\n";
                                } else {
                                    response = "$-1\r\n";  // nil
                                }

                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "HDEL") {
                        if (tokens.size() < 3) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            std::string key = tokens[1];
                            std::vector<std::string> fields(tokens.begin() + 2, tokens.end());

                            try {
                                int removed = store.hdel(key, fields);
                                response = ":" + std::to_string(removed) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "HLEN") {
                        if (tokens.size() != 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                int size = store.hlen(tokens[1]);
                                response = ":" + std::to_string(size) + "\r\n";
                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "HGETALL") {
                        if (tokens.size() != 2) {
                            response = "-ERR wrong number of arguments\r\n";
                        } else {
                            try {
                                auto result = store.hgetall(tokens[1]);

                                response = "*" + std::to_string(result.size() * 2) + "\r\n";

                                for (auto& [k, v] : result) {
                                    response += "$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
                                    response += "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
                                }

                            } catch (...) {
                                response = "-ERR WRONGTYPE\r\n";
                            }
                        }
                    }
                    else if (command == "PING") {
                        response = "+PONG\r\n";
                    }
                    else if (command == "FLUSHALL") {
                        store.flushAll();
                        response = "+OK\r\n";
                    }
                    else if (command == "CONFIG") {
                        if (tokens.size() == 3 && tokens[1] == "GET" && tokens[2] == "maxmemory") {

                            std::string val = std::to_string(store.getMaxMemory());

                            response = "*2\r\n";
                            response += "$9\r\nmaxmemory\r\n";
                            response += "$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
                        }
                        else {
                            response = "-ERR unknown config\r\n";
                        }
                    }
                    else if (command == "INFO") {

                        std::string info;

                        info += "used_memory:" + std::to_string(store.getCurrentMemory()) + "\r\n";
                        info += "max_memory:" + std::to_string(store.getMaxMemory()) + "\r\n";
                        info += "connected_clients:" + std::to_string(connected_clients.load()) + "\r\n";
                        info += "total_commands:" + std::to_string(total_commands.load()) + "\r\n";

                        response = "$" + std::to_string(info.size()) + "\r\n" + info + "\r\n";
                    }
                    else {
                        response = "-ERR unknown command\r\n";
                    }

                } catch (...) {
                    response = "-ERR execution error\r\n";
                }

                send(fd, response.c_str(), response.size(), 0);
            }
        }
    }
}