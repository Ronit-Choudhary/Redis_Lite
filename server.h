#ifndef SERVER_H
#define SERVER_H

#include "store.h"

class TcpServer {
private:
    int server_fd;
    int port;
    bool running;
    std::atomic<int> connected_clients{0};
    std::atomic<long long> total_commands{0};

    Store store;  // ✅ KEEP THIS

    void createSocket();
    void bindSocket();
    void listenSocket();

    void eventLoop();  // ✅ NEW

public:
    TcpServer(int p);
    ~TcpServer();

    void start();
    void stop();
};

#endif