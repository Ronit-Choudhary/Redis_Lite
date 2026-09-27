#include <iostream>
#include <algorithm>   
#include <sstream>    
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(6379);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    if (connect(sock, (sockaddr*)&server, sizeof(server)) < 0) {
        std::cerr << "Connection failed\n";
        return 1;
    }

    std::cout << "Connected to Redis-Lite CLI\n";

    while (true) {
        std::cout << "> ";
        std::string input;
        std::getline(std::cin, input);

        if (input == "exit") break;

        // convert to RESP
        std::string resp = "*" + std::to_string(std::count(input.begin(), input.end(), ' ') + 1) + "\r\n";

        std::stringstream ss(input);
        std::string word;

        while (ss >> word) {
            resp += "$" + std::to_string(word.size()) + "\r\n" + word + "\r\n";
        }

        send(sock, resp.c_str(), resp.size(), 0);

        char buffer[1024];
        int bytes = recv(sock, buffer, sizeof(buffer), 0);

        if (bytes > 0) {
            std::string resp(buffer, bytes);

            if (resp[0] == '$') {
                int pos = resp.find("\r\n");
                std::string value = resp.substr(pos + 2);
                std::cout << value;
            }
            else {
                std::cout << resp;
            }
        }
    }

    close(sock);
    return 0;
}