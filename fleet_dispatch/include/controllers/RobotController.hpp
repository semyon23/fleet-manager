
#ifndef ROBOTCONTROLLER_HPP
#define ROBOTCONTROLLER_HPP

#include <string>
#include <behaviortree_cpp/behavior_tree.h>


class RobotController {
public:
    RobotController(std::string id);

private:
    std::string robot_id;

};



#endif //ROBOTCONTROLLER_HPP
