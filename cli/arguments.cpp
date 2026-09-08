#include "arguments.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <termios.h>
#include <unistd.h>

Arguments parseArgs(int argc, char* argv[], int start) {
    Arguments result;
    for (int i = start; i < argc; ++i) {
        std::string value = argv[i];
        if (value == "--overwrite")
            result.flags.push_back(value);
        else if (value == "-o" || value == "-d" || value == "--pack" || value == "--compress" ||
                 value == "--encrypt" || value == "--key-file" || value == "--server" ||
                 value == "--username" || value == "--account-password-file" || value == "--name") {
            if (i + 1 >= argc)
                throw std::runtime_error("missing value for option: " + value);
            result.options[value] = argv[++i];
        } else if (!value.empty() && value[0] == '-') {
            throw std::runtime_error("unknown option: " + value);
        } else
            result.positional.push_back(value);
    }
    return result;
}

std::string requiredOption(const std::map<std::string, std::string>& options,
                           const std::string& name) {
    auto it = options.find(name);
    if (it == options.end() || it->second.empty())
        throw std::runtime_error("required option is missing: " + name);
    return it->second;
}

bool hasFlag(const Arguments& args, const std::string& flag) {
    return std::find(args.flags.begin(), args.flags.end(), flag) != args.flags.end();
}

std::pair<std::string, uint16_t> parseServer(const std::string& address) {
    auto colon = address.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == address.size())
        throw std::runtime_error("server must use HOST:PORT format");
    unsigned long port = std::stoul(address.substr(colon + 1));
    if (port == 0 || port > 65535)
        throw std::runtime_error("server port is out of range");
    std::string host = address.substr(0, colon);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    return {host, static_cast<uint16_t>(port)};
}

namespace {
std::string readFileSecret(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open password file: " + path);
    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    if (value.empty())
        throw std::runtime_error("password file is empty: " + path);
    return value;
}

std::string promptSecret(const std::string& prompt) {
    std::cerr << prompt;
    termios oldState{};
    bool hidden = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &oldState) == 0;
    if (hidden) {
        termios state = oldState;
        state.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &state);
    }
    std::string value;
    std::getline(std::cin, value);
    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldState);
        std::cerr << '\n';
    }
    if (value.empty())
        throw std::runtime_error("password cannot be empty");
    return value;
}

} // namespace

std::string secretFrom(const std::map<std::string, std::string>& options, const std::string& key,
                       const std::string& prompt) {
    auto it = options.find(key);
    return it == options.end() ? promptSecret(prompt) : readFileSecret(it->second);
}
