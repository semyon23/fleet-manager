#pragma once
#ifndef OPCSERIALBRIDGE_LOGGER_HPP
#define OPCSERIALBRIDGE_LOGGER_HPP

#ifdef SPDLOG_ACTIVE_LEVEL
#  undef SPDLOG_ACTIVE_LEVEL
#endif
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG


#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>
#include <memory>

class custom_level_formatter : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg &msg, const std::tm &, spdlog::memory_buf_t &dest) override {
        std::string level_str;
        switch (msg.level) {
        case spdlog::level::trace:    level_str = "TRACE"; break;
        case spdlog::level::debug:    level_str = "DEBUG"; break;
        case spdlog::level::info:     level_str = "INFO"; break;
        case spdlog::level::warn:     level_str = "WARN"; break;
        case spdlog::level::err:      level_str = "ERROR"; break;
        case spdlog::level::critical: level_str = "CRITICAL"; break;
        default:                      level_str = "OFF"; break;
        }
        dest.append(level_str.data(), level_str.data() + level_str.size());
    }

    // ИСПРАВЛЕНО: добавлен const и используется стандартный std::make_unique
    std::unique_ptr<spdlog::custom_flag_formatter> clone() const override {
        return std::make_unique<custom_level_formatter>();
    }
};

namespace Logger {
    bool init() {
        try {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

            console_sink->set_color(spdlog::level::trace,    "\033[37m");
            console_sink->set_color(spdlog::level::debug,    "\033[36m");
            console_sink->set_color(spdlog::level::info,     "\033[38m");
            console_sink->set_color(spdlog::level::warn,     "\033[33m");
            console_sink->set_color(spdlog::level::err,      "\033[31m");
            console_sink->set_color(spdlog::level::critical, "\033[1;31m");

            auto logger = std::make_shared<spdlog::logger>("global_logger", console_sink);

            auto formatter = std::make_unique<spdlog::pattern_formatter>();
            formatter->add_flag<custom_level_formatter>('L');

            //formatter->set_pattern("\033[32m[%Y-%m-%d %H:%M:%S.%e]\033[m %^[%L] [%n] %v%$");
            formatter->set_pattern("\033[32m[%Y-%m-%d %H:%M:%S.%e]\033[m %^[%L] %v%$");
            logger->set_formatter(std::move(formatter));

            logger->set_level(spdlog::level::trace);
            spdlog::set_default_logger(logger);
            return true;
        }
        catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Log initialization failed: " << ex.what() << std::endl;
            return false;
        }
    };
}


// namespace Logger {
//     bool init() {
//         try {
//             auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
//
//             console_sink->set_color(spdlog::level::trace,    "\033[37m");
//             console_sink->set_color(spdlog::level::debug,    "\033[36m");
//             console_sink->set_color(spdlog::level::info,     "\033[38m");
//             console_sink->set_color(spdlog::level::warn,     "\033[33m");
//             console_sink->set_color(spdlog::level::err,      "\033[31m");
//             console_sink->set_color(spdlog::level::critical, "\033[1;31m");
//
//             auto logger = std::make_shared<spdlog::logger>("global_logger", console_sink);
//
//            // logger->set_pattern("\033[32m[%Y-%m-%d %H:%M:%S.%e]\033[m%^%v%$");
//             logger->set_pattern("\033[32m[%Y-%m-%d %H:%M:%S.%e]\033[m %^[%L] %v%$");
//             logger->set_level(spdlog::level::trace);
//             spdlog::set_default_logger(logger);
//             return true;
//         }
//         catch (const spdlog::spdlog_ex& ex) {
//             std::cerr << "Log initialization failed: " << ex.what() << std::endl;
//             return false;
//         }
//     };
// }
#endif //OPCSERIALBRIDGE_LOGGER_HPP
