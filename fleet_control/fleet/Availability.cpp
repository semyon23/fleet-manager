#include "Availability.hpp"
#include <format>

namespace dbv1 = fms::database::v1;
namespace fms1 = fms::v1;

namespace {

void add_reason(dbv1::RobotAvailability& availability, const char* code, std::string message) {
    auto* reason = availability.add_reasons();
    reason->set_code(code);
    reason->set_message(std::move(message));
}

// "CONNECTION_STATE_STALE" → "STALE"
std::string short_name(const std::string& name, std::string_view prefix) {
    return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

}  // namespace

// Правила по порядку; первое сработавшее задаёт state (таблица — в README, раздел Availability)
dbv1::RobotAvailability availability_of(const fms1::RobotStatus* status, double low_battery_percent) {
    dbv1::RobotAvailability availability;

    // Связь с роботом
    if (!status) {
        availability.set_state(dbv1::AVAILABILITY_STATE_OFFLINE);
        add_reason(availability, "NO_CONNECTION", "nothing has been received from the robot yet");
        return availability;
    }
    if (status->connection() != fms1::CONNECTION_STATE_ONLINE) {
        availability.set_state(dbv1::AVAILABILITY_STATE_OFFLINE);
        add_reason(availability, "NO_CONNECTION",
                   "connection: " + short_name(fms1::ConnectionState_Name(status->connection()), "CONNECTION_STATE_"));
        return availability;
    }

    // Ошибка или аварийная остановка
    for (const auto& error : status->errors()) {
        if (error.level() == fms1::ERROR_LEVEL_CRITICAL || error.level() == fms1::ERROR_LEVEL_FATAL) {
            add_reason(availability, "FATAL_ERROR",
                       error.error_type() + (error.description().empty() ? "" : ": " + error.description()));
        }
    }
    auto stop = status->safety().emergency_stop();
    if (stop == fms1::EMERGENCY_STOP_MANUAL || stop == fms1::EMERGENCY_STOP_REMOTE) {
        add_reason(availability, "EMERGENCY_STOP",
                   "emergency stop: " + short_name(fms1::EmergencyStop_Name(stop), "EMERGENCY_STOP_"));
    }
    if (availability.reasons_size() > 0) {
        availability.set_state(dbv1::AVAILABILITY_STATE_ERROR);
        return availability;
    }

    // Пауза и миссия
    if (status->paused()) {
        availability.set_state(dbv1::AVAILABILITY_STATE_PAUSED);
        return availability;
    }
    if (!status->mission_id().empty()) {
        availability.set_state(dbv1::AVAILABILITY_STATE_BUSY);
        return availability;
    }

    // Готов ли брать работу — все причины сразу, чтобы оператор видел, что исправлять
    if (!status->has_factsheet_received_at()) {
        add_reason(availability, "NO_FACTSHEET", "factsheet has not been received, robot capabilities are unknown");
    }
    auto mode = status->operating_mode();
    if (mode == fms1::OPERATING_MODE_STARTUP) {
        add_reason(availability, "STARTUP", "robot is starting up");
    } else if (mode != fms1::OPERATING_MODE_AUTOMATIC && mode != fms1::OPERATING_MODE_SEMIAUTOMATIC) {
        // SEMIAUTOMATIC: по VDA 5050 робот выполняет order'а, скорость задаёт оператор — работать может
        add_reason(availability, "MODE_NOT_AUTOMATIC",
                   "operating mode: " + short_name(fms1::OperatingMode_Name(mode), "OPERATING_MODE_"));
    }
    if (!status->localized()) {
        add_reason(availability, "NOT_LOCALIZED", "robot is not localized");
    }
    if (status->last_node_id().empty()) {   // пока без карт: только «стоит на каком-то узле»
        add_reason(availability, "NOT_ON_GRAPH", "robot is not on a graph node, set its position first");
    }
    const auto& power = status->power();
    if (!power.charging() && power.state_of_charge() < low_battery_percent) {
        add_reason(availability, "LOW_BATTERY",
                   std::format("battery {:.0f} % is below {:.0f} %", power.state_of_charge(), low_battery_percent));
    }
    if (availability.reasons_size() > 0) {
        availability.set_state(dbv1::AVAILABILITY_STATE_NOT_READY);
        return availability;
    }

    availability.set_state(power.charging() ? dbv1::AVAILABILITY_STATE_CHARGING : dbv1::AVAILABILITY_STATE_IDLE);
    return availability;
}