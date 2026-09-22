
#ifndef TYPE_HPP
#define TYPE_HPP

#include <string>

struct  AgvPosition {
    std::string map_id;
    bool position_initialized;
    double x;
    double y ;
    double theta;
};

struct Battery_State {
    double battery_charge;
    bool charging;
};

struct SafetyState {
    std::string e_stop;
    bool field_violation;
};


struct RobotVisualization {
    std::string robot_id;
    AgvPosition agv_position;
};

struct RegisterRobot{
    std::string robot_id;
    std::string manufacturer;
};

struct UnregisterRobot{
    std::string robot_id;
};

struct RobotStatusUpdate {
    std::string robot_id;
    std::string status;
    double battery_level;

    unsigned int header_id;
    std::string timestamp;
    std::string version;
    std::string manufacturer;
    std::string serial_number;

    std::string order_id;
    unsigned int order_update_id;
    std::string last_node_id;
    unsigned int last_node_sequence_id;

    std::string operating_mode;
    bool driving;

    Battery_State battery_state;
    SafetyState safety_state;

    std::string  action_states;
    std::string  edge_states;
    std::string node_states;
    std::string  errors;
    std::string  information;
    std::string  loads;
};



#endif //TYPE_HPP
