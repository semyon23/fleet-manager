#include "Availability.hpp"
#include <format>

namespace dbv1 = fms::database::v1;
namespace fms1 = fms::v1;

// Дальше этого от узла — робот не на узле, а между узлами (остановился посреди ребра после отмены).
// С запасом на погрешность локализации; у VDA-роботов точность остановки — сантиметры
constexpr double kOnNodeTolerance = 0.5;   // м

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
dbv1::RobotAvailability availability_of(const fms::v1::RobotStatus* status, double low_battery_percent, const MapStore& maps) {
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
    // M-2. На графе: карта робота (pose.mapId) есть в fms, lastNodeId — её узел,
    // и робот стоит у этого узла, а не посреди ребра. Пока карты не загружены — только «стоит на каком-то узле».
    const std::string& last_node = status->last_node_id();
    if (maps.loaded() && status->has_pose()) {
        const std::string& map_id = status->pose().map_id();
        const Graph* graph = maps.find(map_id);
        if (!graph) {
            add_reason(availability, "UNKNOWN_MAP",
                       map_id.empty() ? "robot reports no mapId"
                                      : "robot map " + map_id + " is not in fms: create the map with this id");
        } else if (last_node.empty() || !graph->find_node(last_node)) {
            add_reason(availability, "NOT_ON_GRAPH",
                       "robot is not on a node of map " + map_id + ", set its position first");
        } else {
            const GraphNode& node = graph->nodes[*graph->find_node(last_node)];
            const double away = std::hypot(status->pose().x() - node.x, status->pose().y() - node.y);
            if (away > kOnNodeTolerance) {
                add_reason(availability, "NOT_ON_GRAPH",
                           std::format("robot is {:.1f} m away from node {}: it stopped between nodes, "
                                       "drive it to a node or set its position", away, last_node));
            }
        }
    } else if (last_node.empty()) {
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