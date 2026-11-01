
#include "../include/api_json.hpp"
#include <chrono>
#include <format>
#include <map>
#include <google/protobuf/util/time_util.h>

using google::protobuf::util::TimeUtil;
using nlohmann::json;
namespace dbv1 = fms::database::v1;

namespace {

// "CONNECTION_STATE_ONLINE" → "ONLINE"
std::string enum_name(const std::string& name, std::string_view prefix) {
    return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

// 2026-10-14T10:15:00.123Z
std::string iso_time(const google::protobuf::Timestamp& time) {
    std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::milliseconds{TimeUtil::TimestampToMilliseconds(time)}};
    return std::format("{:%FT%T}Z", point);
}

json time_or_null(bool present, const google::protobuf::Timestamp& time) {
    return present ? json(iso_time(time)) : json(nullptr);
}

// map из proto → объект JSON; пустой — {}, а не null
json string_map(const google::protobuf::Map<std::string, std::string>& map) {
    return json(std::map<std::string, std::string>(map.begin(), map.end()));
}

json status_json(const fms::v1::RobotStatus& status) {
    json errors = json::array();
    for (const auto& error : status.errors()) {
        errors.push_back({
            {"errorType", error.error_type()},
            {"level", enum_name(fms::v1::ErrorLevel_Name(error.level()), "ERROR_LEVEL_")},
            {"description", error.description()},
            {"hint", error.hint()},
            {"references", string_map(error.references())},
        });
    }

    json loads = json::array();
    for (const auto& load : status.loads()) {
        loads.push_back({{"loadId", load.load_id()}, {"loadType", load.load_type()}});
    }

    const auto& pose = status.pose();
    const auto& power = status.power();
    return {
        {"connection", enum_name(fms::v1::ConnectionState_Name(status.connection()), "CONNECTION_STATE_")},
        {"operatingMode", enum_name(fms::v1::OperatingMode_Name(status.operating_mode()), "OPERATING_MODE_")},
        {"pose", status.has_pose()
                     ? json{{"x", pose.x()}, {"y", pose.y()}, {"theta", pose.theta()}, {"mapId", pose.map_id()}}
                     : json(nullptr)},
        {"localized", status.localized()},
        {"lastNodeId", status.last_node_id()},
        {"driving", status.driving()},
        {"paused", status.paused()},
        {"power", {
            {"stateOfCharge", power.state_of_charge()},
            {"charging", power.charging()},
            {"voltage", power.has_voltage() ? json(power.voltage()) : json(nullptr)},
            {"range", power.has_range() ? json(power.range()) : json(nullptr)},
        }},
        {"safety", {
            {"emergencyStop", enum_name(fms::v1::EmergencyStop_Name(status.safety().emergency_stop()), "EMERGENCY_STOP_")},
            {"fieldViolation", status.safety().field_violation()},
        }},
        {"errors", errors},
        {"loads", loads},
        {"missionId", status.mission_id().empty() ? json(nullptr) : json(status.mission_id())},
        {"factsheetReceivedAt", time_or_null(status.has_factsheet_received_at(), status.factsheet_received_at())},
        {"lastStateAt", time_or_null(status.has_last_state_at(), status.last_state_at())},
    };
}

}  // namespace

json robot_json(const dbv1::RobotRecord& record) {
    const auto& spec = record.spec();
    return {
        {"id", spec.robot_id()},
        {"name", spec.name().empty() ? spec.robot_id() : spec.name()},   // по контракту: по умолчанию = id
        {"manufacturer", spec.manufacturer()},
        {"serialNumber", spec.serial_number()},
        {"labels", string_map(spec.labels())},
        {"lifecycle", enum_name(dbv1::RobotLifecycle_Name(spec.lifecycle()), "ROBOT_LIFECYCLE_")},
        {"createdAt", iso_time(spec.created_at())},
        {"updatedAt", iso_time(spec.updated_at())},
        {"status", record.has_status() ? status_json(record.status()) : json(nullptr)},
        {"availability", nullptr},    // считает control — появится вместе с ним
        {"graphPosition", nullptr},   // тоже control
        {"info", nullptr},            // выжимка из factsheet — когда понадобится карточке робота
    };
}