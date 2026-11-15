#include "AlarmRules.hpp"

#include <format>
#include <map>

namespace fms1 = fms::v1;

namespace {

constexpr int kOfflineDelayMs = 5000;

std::string without_prefix(const std::string& name, std::string_view prefix) {
    return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

// Уровень ошибки VDA → severity (контракт 8.1). Не указан — как WARNING
int rank(fms1::ErrorLevel level) {
    switch (level) {
        case fms1::ERROR_LEVEL_URGENT: return 1;
        case fms1::ERROR_LEVEL_CRITICAL:
        case fms1::ERROR_LEVEL_FATAL: return 2;
        default: return 0;
    }
}
constexpr const char* kSeverityByRank[] = {"WARNING", "ERROR", "CRITICAL"};

}  // namespace

std::vector<AlarmCondition> robot_alarm_conditions(const std::string& robot_id, const fms1::RobotStatus* status,
                                                   double low_battery_percent) {
    const auto connection = status ? status->connection() : fms1::CONNECTION_STATE_UNKNOWN;
    if (connection == fms1::CONNECTION_STATE_HIBERNATING) {
        return {};
    }
    if (connection != fms1::CONNECTION_STATE_ONLINE) {
        return {{"ROBOT_OFFLINE/" + robot_id, "ROBOT_OFFLINE", "ERROR", "Robot offline",
                 std::format("{}: connection {}", robot_id,
                             without_prefix(fms1::ConnectionState_Name(connection), "CONNECTION_STATE_")),
                 kOfflineDelayMs}};
    }

    if (!status->has_last_state_at()) {
        return {};   // на связи, но state ещё не было: заряд 0 и пустые поля — не данные робота
    }

    std::vector<AlarmCondition> conditions;
    const auto stop = status->safety().emergency_stop();
    if (stop == fms1::EMERGENCY_STOP_MANUAL || stop == fms1::EMERGENCY_STOP_REMOTE) {
        conditions.push_back({"EMERGENCY_STOP/" + robot_id, "EMERGENCY_STOP", "CRITICAL", "Emergency stop",
                              std::format("{}: emergency stop ({})", robot_id,
                                          without_prefix(fms1::EmergencyStop_Name(stop), "EMERGENCY_STOP_"))});
    }

    // Одна тревога на errorType; одинаковых ошибок с разным уровнем — по самому серьёзному
    std::map<std::string, const fms1::RobotError*> errors;
    for (const auto& error : status->errors()) {
        auto [it, inserted] = errors.emplace(error.error_type(), &error);
        if (!inserted && rank(error.level()) > rank(it->second->level())) {
            it->second = &error;
        }
    }
    for (const auto& [type, error] : errors) {
        conditions.push_back({std::format("ROBOT_ERROR/{}/{}", robot_id, type), "ROBOT_ERROR",
                              kSeverityByRank[rank(error->level())], "Robot error: " + type,
                              std::format("{}: {}", robot_id, error->description().empty() ? type : error->description())});
    }

    const auto& power = status->power();
    if (!power.charging() && power.state_of_charge() < low_battery_percent) {
        conditions.push_back({"LOW_BATTERY/" + robot_id, "LOW_BATTERY", "WARNING", "Low battery",
                       std::format("{}: battery {:.0f} % is below {:.0f} %", robot_id, power.state_of_charge(),
                                   low_battery_percent),
                       10000});
    }
    return conditions;
}