#include "server.h"
#include <iostream>

int main() {
    try {
        TcpServer server(6379);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}