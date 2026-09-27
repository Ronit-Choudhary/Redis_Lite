#include "resp_parser.h"
#include <stdexcept>

int readNumber(const std::string& s, int& i) {
    int num = 0;

    while (i < s.size() && s[i] != '\r') {
        if (!isdigit(s[i])) {
            throw std::runtime_error("Invalid number");
        }
        num = num * 10 + (s[i] - '0');
        i++;
    }

    return num;
}

std::vector<std::string> parseRESP(const std::string& input) {
    std::vector<std::string> result;
    int i = 0;

    if (input[i] != '*') {
        throw std::runtime_error("Expected array");
    }

    i++; // skip '*'

    int num = readNumber(input, i);

    i += 2; // skip \r\n

    while (num--) {
        if (input[i] != '$') {
            throw std::runtime_error("Expected bulk string");
        }

        i++; // skip '$'

        int len = readNumber(input, i);

        i += 2; // skip \r\n

        std::string word = input.substr(i, len);
        result.push_back(word);

        i += len + 2; // skip data + \r\n
    }

    return result;
}