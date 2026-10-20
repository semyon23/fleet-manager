
#include "../include/RobotStatusBuilder.hpp"


namespace {

fms::v1::OperatingMode to_proto(vda5050::OperatingMode mode) {
    switch (mode) {
        case vda5050::OperatingMode::Startup:       return fms::v1::OPERATING_MODE_STARTUP;
        case vda5050::OperatingMode::Automatic:     return fms::v1::OPERATING_MODE_AUTOMATIC;
        case vda5050::OperatingMode::Semiautomatic: return fms::v1::OPERATING_MODE_SEMIAUTOMATIC;
        case vda5050::OperatingMode::Intervened:    return fms::v1::OPERATING_MODE_INTERVENED;
        case vda5050::OperatingMode::Manual:        return fms::v1::OPERATING_MODE_MANUAL;
        case vda5050::OperatingMode::Service:       return fms::v1::OPERATING_MODE_SERVICE;
        case vda5050::OperatingMode::TeachIn:       return fms::v1::OPERATING_MODE_TEACH_IN;
    }
    return fms::v1::OPERATING_MODE_UNSPECIFIED;
}

fms::v1::ErrorLevel to_proto(vda5050::ErrorLevel level) {
    switch (level) {
        case vda5050::ErrorLevel::Warning:  return fms::v1::ERROR_LEVEL_WARNING;
        case vda5050::ErrorLevel::Urgent:   return fms::v1::ERROR_LEVEL_URGENT;
        case vda5050::ErrorLevel::Critical: return fms::v1::ERROR_LEVEL_CRITICAL;
        case vda5050::ErrorLevel::Fatal:    return fms::v1::ERROR_LEVEL_FATAL;
    }
    return fms::v1::ERROR_LEVEL_UNSPECIFIED;
}

fms::v1::EmergencyStop to_proto(vda5050::EmergencyStop stop) {
    switch (stop) {
        case vda5050::EmergencyStop::None:   return fms::v1::EMERGENCY_STOP_NONE;
        case vda5050::EmergencyStop::Manual: return fms::v1::EMERGENCY_STOP_MANUAL;
        case vda5050::EmergencyStop::Remote: return fms::v1::EMERGENCY_STOP_REMOTE;
    }
    return fms::v1::EMERGENCY_STOP_UNSPECIFIED;
}

}  // namespace

fms::v1::ConnectionState to_proto(vda5050::ConnectionState state) {
    switch (state) {
        case vda5050::ConnectionState::Online:           return fms::v1::CONNECTION_STATE_ONLINE;
        case vda5050::ConnectionState::Offline:          return fms::v1::CONNECTION_STATE_OFFLINE;
        case vda5050::ConnectionState::Hibernating:      return fms::v1::CONNECTION_STATE_HIBERNATING;
        case vda5050::ConnectionState::ConnectionBroken: return fms::v1::CONNECTION_STATE_CONNECTION_BROKEN;
    }
    return fms::v1::CONNECTION_STATE_UNSPECIFIED;
}

void fill_status(const vda5050::State& state, fms::v1::RobotStatus& status) {
    status.set_operating_mode(to_proto(state.operating_mode));

    if (state.mobile_robot_position) {
        const auto& position = *state.mobile_robot_position;
        auto* pose = status.mutable_pose();
        pose->set_x(position.x);
        pose->set_y(position.y);
        pose->set_theta(position.theta);
        pose->set_map_id(position.map_id);
        status.set_localized(position.localized);
    } else {
        status.clear_pose();
        status.set_localized(false);
    }

    status.set_last_node_id(state.last_node_id);
    status.set_driving(state.driving);
    status.set_paused(state.paused.value_or(false));

    auto* power = status.mutable_power();
    power->set_state_of_charge(state.power_supply.state_of_charge);
    power->set_charging(state.power_supply.charging);
    if (state.power_supply.battery_voltage) {
        power->set_voltage(*state.power_supply.battery_voltage);
    } else {
        power->clear_voltage();
    }
    if (state.power_supply.range) {
        power->set_range(*state.power_supply.range);
    } else {
        power->clear_range();
    }

    auto* safety = status.mutable_safety();
    safety->set_emergency_stop(to_proto(state.safety_state.active_emergency_stop));
    safety->set_field_violation(state.safety_state.field_violation);

    status.clear_errors();
    for (const auto& error : state.errors) {
        auto* out = status.add_errors();
        out->set_error_type(error.error_type);
        out->set_level(to_proto(error.error_level));
        out->set_description(error.error_description.value_or(""));
        out->set_hint(error.error_hint.value_or(""));
        if (error.error_references) {
            for (const auto& reference : *error.error_references) {
                (*out->mutable_references())[reference.reference_key] = reference.reference_value;
            }
        }
    }

    status.clear_loads();
    if (state.loads) {
        for (const auto& load : *state.loads) {
            auto* out = status.add_loads();
            out->set_load_id(load.load_id.value_or(""));
            out->set_load_type(load.load_type.value_or(""));
        }
    }
}