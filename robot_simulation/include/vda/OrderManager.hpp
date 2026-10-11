
#ifndef ORDERMANAGER_HPP
#define ORDERMANAGER_HPP

#include "../vda5050/vda5050.hpp"
#include <optional>
#include <map>
#include <set>

class OrderManager {
public:
    OrderManager(vda5050::State& State, vda5050::Order& Order);

    void update(const std::string& json_order);

    void cancel();
    void set_supported_actions(const std::vector<vda5050::MobileRobotAction>& actions);
private:
    bool is_idle() const;
    bool is_start_node_in_range(const vda5050::Node& node) const;
    void append_states(const vda5050::Order& order, std::size_t first_node_index);
    const vda5050::Node* decision_point() const;
    void drop_horizon();
    void clear_order_errors();
    struct Rejection {
        std::string type;
        vda5050::ErrorLevel level = vda5050::ErrorLevel::Warning;
        std::string description;
        std::vector<vda5050::ErrorReference> references;
    };
    std::optional<vda5050::Order> parse_order(const std::string& json_order);
    std::optional<Rejection> validate_structure(const vda5050::Order& order) const;
    std::optional<Rejection> validate_capabilities(const vda5050::Order& order) const;
    vda5050::State& State_;
    vda5050::Order& Order_;
    vda5050::Order Last_received_order;

    void reject(const Rejection& r, const vda5050::Order* order);

    std::map<std::string, std::set<vda5050::ActionScope>> action_scopes_;
    bool order_canceled{false};
};



#endif //ORDERMANAGER_HPP
