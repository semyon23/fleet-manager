#include "../../include/utils/RobotTopicManager.hpp"

void RobotTopicManager::add_topic(const std::string& robot_id, const std::string& topic){
    auto range = robot_map.equal_range(robot_id);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == topic) {
            std::println(stderr,"The topic ({}) already exists", topic);
            return;
        }
    }
    robot_map.insert({robot_id, topic});
}

void RobotTopicManager::delete_robot_topics(const std::string& robot_id){
    size_t removed_count = robot_map.erase(robot_id);
    std::println(stderr,"The topic ({}) removed, count {}", robot_id, removed_count);
}

std::vector<std::string> RobotTopicManager::get_robot_topics(const std::string& robot_id){
    std::vector<std::string> topics;

    auto range = robot_map.equal_range(robot_id);
    for (auto it = range.first; it != range.second; ++it) {
        topics.push_back(it->second);
    }
    return topics;
}
