#pragma once

#include <string>

namespace server::core::log {

enum class level { trace, debug, info, warn, error };

void set_level(level lv);
void trace(const std::string& msg);
void debug(const std::string& msg);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

} // namespace server::core::log

