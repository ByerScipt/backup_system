#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>
struct Arguments {
    std::vector<std::string> positional;
    std::map<std::string,std::string> options;
    std::vector<std::string> flags;
};
Arguments parseArgs(int argc, char* argv[], int start);
std::string requiredOption(const std::map<std::string,std::string>& options, const std::string& name);
bool hasFlag(const Arguments& args, const std::string& flag);
std::pair<std::string,uint16_t> parseServer(const std::string& address);
std::string readFileSecret(const std::string& path);
std::string promptSecret(const std::string& prompt);
std::string secretFrom(const std::map<std::string,std::string>& options, const std::string& key, const std::string& prompt);
