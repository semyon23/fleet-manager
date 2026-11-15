#include "RobotEvents.hpp"

#include <map>

namespace fms1 = fms::v1;
namespace dbv1 = fms::database::v1;
using nlohmann::json;

namespace {

std::string without_prefix(const std::string& name, std::string_view prefix) {
    return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

std::string level_name(fms1::ErrorLevel level) {
    return without_prefix(fms1::ErrorLevel_Name(level), "ERROR_LEVEL_");
}

// errorType + уровень → ошибка: тот же тип с другим уровнем — другая ошибка
std::map<std::string, const fms1::RobotError*> errors_of(const fms1::RobotStatus* status) {
    std::map<std::string, const fms1::RobotError*> errors;
    if (status) {
        for (const auto& error : status->errors()) {
            errors.emplace(error.error_type() + "/" + level_name(error.level()), &error);
        }
    }
    return errors;
}

}  // namespace

std::vector<JournalEvent> robot_status_events(const fms1::RobotStatus* before, const fms1::RobotStatus& after) {
    std::vector<JournalEvent> events;

    const auto was = before ? before->connection() : fms1::CONNECTION_STATE_UNSPECIFIED;
    const bool known = after.connection() != fms1::CONNECTION_STATE_UNSPECIFIED &&
                       after.connection() != fms1::CONNECTION_STATE_UNKNOWN;
    if (after.connection() != was && known) {
        events.push_back({"CONNECTION",
                          {{"state", without_prefix(fms1::ConnectionState_Name(after.connection()), "CONNECTION_STATE_")}}});
    }

    const auto old_errors = errors_of(before);
    const auto new_errors = errors_of(&after);
    for (const auto& [key, error] : new_errors) {
        if (!old_errors.contains(key)) {
            events.push_back({"ERROR", {{"errorType", error->error_type()}, {"level", level_name(error->level())},
                                        {"description", error->description()}, {"cleared", false}}});
        }
    }
    for (const auto& [key, error] : old_errors) {
        if (!new_errors.contains(key)) {
            events.push_back({"ERROR", {{"errorType", error->error_type()}, {"level", level_name(error->level())},
                                        {"description", error->description()}, {"cleared", true}}});
        }
    }
    return events;
}

std::optional<JournalEvent> availability_event(const dbv1::RobotAvailability* before,
                                               const dbv1::RobotAvailability& after) {
    if (!before || before->state() == after.state()) {
        return std::nullopt;
    }
    json reasons = json::array();
    for (const auto& reason : after.reasons()) {
        reasons.push_back({{"code", reason.code()}, {"message", reason.message()}});
    }
    return JournalEvent{"AVAILABILITY",
                        {{"state", without_prefix(dbv1::AvailabilityState_Name(after.state()), "AVAILABILITY_STATE_")},
                         {"reasons", reasons}}};
}