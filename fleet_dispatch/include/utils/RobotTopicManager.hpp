#ifndef ROBOTTOPICMANAGER_HPP
#define ROBOTTOPICMANAGER_HPP

#include <unordered_map>
#include <string>
#include <print>
#include <vector>

class RobotTopicManager {
public:
    void add_topic(const std::string& robot_id, const std::string& topic);
    std::vector<std::string> get_robot_topics(const std::string& robot_id);
    void delete_robot_topics(const std::string& robot_id);
private:
    std::unordered_multimap<std::string, std::string> robot_map;
};



#endif //ROBOTTOPICMANAGER_HPP
