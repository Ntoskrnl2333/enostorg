#pragma once

#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace enostorg {

class Config {
public:
    bool load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        std::string line, section;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line[0] == '[') {
                size_t end = line.find(']');
                if (end == std::string::npos) continue;
                section = trim(line.substr(1, end - 1));
                continue;
            }

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));
            size_t comment = value.find_first_of(";#");
            if (comment != std::string::npos)
                value = trim(value.substr(0, comment));

            std::string fullKey = section.empty() ? key : section + "." + key;
            data_[fullKey] = value;
        }
        return true;
    }

    std::string get(const std::string& key, const std::string& defaultVal = "") const {
        auto it = data_.find(key);
        return it != data_.end() ? it->second : defaultVal;
    }

    int getInt(const std::string& key, int defaultVal = 0) const {
        auto it = data_.find(key);
        if (it == data_.end()) return defaultVal;
        try { return std::stoi(it->second); }
        catch (...) { return defaultVal; }
    }

    int64_t getInt64(const std::string& key, int64_t defaultVal = 0) const {
        auto it = data_.find(key);
        if (it == data_.end()) return defaultVal;
        try { return std::stoll(it->second); }
        catch (...) { return defaultVal; }
    }

    bool getBool(const std::string& key, bool defaultVal = false) const {
        auto it = data_.find(key);
        if (it == data_.end()) return defaultVal;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }

private:
    std::unordered_map<std::string, std::string> data_;

    static std::string trim(const std::string& s) {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
        return s.substr(start, end - start);
    }
};

} // namespace enostorg
