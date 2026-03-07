#pragma once
#include <iostream>
#include <sstream>

namespace cli {

template<typename... Args>
inline void line(Args&&... args) {
    std::ostringstream ss;
    (ss << ... << args);
    std::cout << ss.str() << std::endl;
}

template<typename... Args>
inline void warn(Args&&... args) {
    std::ostringstream ss;
    (ss << ... << args);
    std::cout << "[WARN] " << ss.str() << std::endl;
}

template<typename... Args>
inline void error(Args&&... args) {
    std::ostringstream ss;
    (ss << ... << args);
    std::cerr << "[ERROR] " << ss.str() << std::endl;
}

} // namespace cli
