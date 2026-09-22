
#ifndef ROBOTCONTROLLER_HPP
#define ROBOTCONTROLLER_HPP

#include <string>
#include <behaviortree_cpp/behavior_tree.h>
#include "include/vda5050/state/RobotState.hpp"

class RobotController {
public:
    RobotController(std::string id, std::string manufacturer);
    ~RobotController();
    std::vector<std::string> get_required_topics() const;
    void update_state(std::string topic_state);
private:
    std::string robot_id;
    std::string robot_manufacturer;
    std::string status_topic;
    std::string visu_topic;

    RobotState robot_state;
};



#endif //ROBOTCONTROLLER_HPP
