#include "server/core/config/dotenv.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace server::core::config {

namespace {
static inline void rtrim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
}
static inline void ltrim(std::string& s) {
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; if (i) s.erase(0, i);
}
static inline void trim(std::string& s) { rtrim(s); ltrim(s); }
static inline std::string unquote(const std::string& v) {
    if (v.size() >= 2) {
        char q = v.front();
        if ((q == '"' || q == '\'') && v.back() == q) {
            return v.substr(1, v.size() - 2);
        }
    }
    return v;
}
static void set_env(const std::string& k, const std::string& v, bool override_existing) {
#if defined(_WIN32)
    if (!override_existing) {
        if (std::getenv(k.c_str())) return;
    }
    _putenv_s(k.c_str(), v.c_str());
#else
    if (!override_existing && std::getenv(k.c_str())) return;
    setenv(k.c_str(), v.c_str(), override_existing ? 1 : 0);
#endif
}
}

bool load_dotenv(const std::string& path, bool override_existing) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        // trim and skip comments/empty
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        // support optional 'export '
        const std::string export_kw = "export ";
        if (line.rfind(export_kw, 0) == 0) line.erase(0, export_kw.size());
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        trim(key); trim(val);
        val = unquote(val);
        if (!key.empty()) set_env(key, val, override_existing);
    }
    return true;
}

} // namespace server::core::config
