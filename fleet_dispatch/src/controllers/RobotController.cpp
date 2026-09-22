#include "../../include/controllers/RobotController.hpp"

RobotController::RobotController(std::string id, std::string manufacturer) : robot_id(id),  robot_manufacturer(manufacturer)
{
    status_topic = std::format("arm/v1/{}/{}/state", robot_id, robot_manufacturer);
    status_topic = std::format("arm/v1/{}/{}/visualization", robot_id, robot_manufacturer);
}

RobotController::~RobotController() {}

std::vector<std::string> RobotController::get_required_topics() const
{
    return { status_topic, visu_topic };
}

void RobotController::update_state(std::string topic_state)
{
    try {
        nlohmann::json json_payload = nlohmann::json::parse(topic_state);
        RobotState incoming_state = json_payload.get<RobotState>();
        this->robot_state = std::move(incoming_state);

        // Дальше здесь вызывается логика вашей стейт-машины и таймеров
        // this->evaluate_mission_progress();

    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Parsing error JSON: " << e.what() << std::endl;
        std::println("Parsing error JSON: {}", e.what());
    } catch (const nlohmann::json::type_error& e) {
        std::println("Structure field validation error JSON: {}", e.what());
    } catch (const nlohmann::json::out_of_range& e) {
        std::println("In JSON Mandatory field is missing : {}", e.what());
    }
}
