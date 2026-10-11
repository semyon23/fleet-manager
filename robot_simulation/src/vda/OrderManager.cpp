#include "include/vda/OrderManager.hpp"
#include <spdlog/spdlog.h>

namespace {
    // Предупреждения о прошлых action'ах: актуальны до следующего принятого order'а.
    const std::set<std::string> kActionWarnings = {
        vda5050::error_type::kNoOrderToCancel,
        vda5050::error_type::kInvalidInstantAction,
        "ACTION_FAILED",   // ActionExecutor::report_failure
    };
}

OrderManager::OrderManager(vda5050::State& State, vda5050::Order& Order) : State_{State}, Order_{Order}{
};

void OrderManager::cancel(){
    order_canceled = true;

    const auto last = State_.last_node_sequence_id;
    Order_.nodes.erase(std::remove_if(Order_.nodes.begin(), Order_.nodes.end(),
                                      [&](const auto& n){ return n.sequence_id > last; }),
                       Order_.nodes.end());
    Order_.edges.erase(std::remove_if(Order_.edges.begin(), Order_.edges.end(),
                                      [&](const auto& e){ return e.sequence_id > last; }),
                       Order_.edges.end());
    State_.node_states.clear();
    State_.edge_states.clear();
    State_.new_base_request = false;
}

void OrderManager::set_supported_actions(const std::vector<vda5050::MobileRobotAction>& actions){
    action_scopes_.clear();
    for (const auto& a : actions){
        action_scopes_[a.action_type].insert(a.action_scopes.begin(), a.action_scopes.end());
    }
}

void OrderManager::reject(const Rejection& r, const vda5050::Order* order){
    using namespace vda5050;

    spdlog::error("[order_manager][{}] Order rejected : type : {}, description :{}", State_.header.serial_number, r.type, r.description);

    Error err;
    err.error_type = r.type;
    err.error_level = r.level;
    err.error_description = r.description;
    auto refs = r.references;
    if (order && std::none_of(refs.begin(), refs.end(), [](const auto& x){ return x.reference_key == "orderId"; })){
        refs.push_back(error_ref("orderId", order->order_id));
    }
    if (!refs.empty()){
        err.error_references = std::move(refs);
    }

    State_.errors.erase(std::remove_if(State_.errors.begin(), State_.errors.end(),
                                       [&](const Error& e){ return e.error_type == r.type; }), State_.errors.end());
    State_.errors.push_back(std::move(err));
}

std::optional<vda5050::Order> OrderManager::parse_order(const std::string& json_order){
    namespace vda = vda5050;
    auto fail = [&](const std::string& why) -> std::optional<vda5050::Order>{
        reject({vda::error_type::kValidationFailure, vda::ErrorLevel::Warning, why, {}}, nullptr);
        return std::nullopt;
    };

    if (json_order.empty()){
        return fail("Empty order message");
    }

    try{
        return vda::json::parse(json_order).get<vda::Order>();
    } catch (const vda::json::parse_error& e){
        return fail(std::string("Order is not valid JSON: ") + e.what());
    } catch (const vda::json::out_of_range& e){
        return fail(std::string("Order misses a mandatory field: ") + e.what());
    } catch (const vda::json::type_error& e){
        return fail(std::string("Order field has wrong type: ") + e.what());
    } catch (const std::invalid_argument& e){
        return fail(std::string("Order has unknown enum value: ") + e.what());
    } catch (const std::exception& e){
        return fail(std::string("Malformed order: ") + e.what());
    }
}

std::optional<OrderManager::Rejection> OrderManager::validate_structure(const vda5050::Order& order) const{
    using namespace vda5050;
    auto fail = [](std::string why){
        return Rejection{error_type::kValidationFailure, ErrorLevel::Warning, std::move(why), {}};
    };

    if (order.nodes.empty()){
        return fail("Order has no nodes");
    }
    if (order.edges.size() != order.nodes.size() - 1){
        return fail("Number of edges must be number of nodes minus one");
    }

    const std::uint32_t base_seq = order.nodes.front().sequence_id;
    if (base_seq % 2 != 0){
        return fail("Node sequenceId must be even");
    }
    for (std::size_t i = 0; i < order.nodes.size(); ++i){
        if (order.nodes[i].sequence_id != base_seq + 2 * i){
            return fail("Node sequenceId is not continuous at nodeId " + order.nodes[i].node_id);
        }
    }
    for (std::size_t i = 0; i < order.edges.size(); ++i){
        if (order.edges[i].sequence_id != base_seq + 2 * i + 1){
            return fail("Edge sequenceId is not continuous at edgeId " + order.edges[i].edge_id);
        }
    }

    if (!order.nodes.front().released){
        return fail("First node must be released");
    }
    bool horizon_started = false;
    for (std::size_t i = 0; i < order.edges.size(); ++i){
        const auto& edge = order.edges[i];
        const auto& next = order.nodes[i + 1];
        if (edge.released && !next.released){
            return fail("Released edge " + edge.edge_id + " leads to unreleased node");
        }
        if (horizon_started && (edge.released || next.released)){
            return fail("Released element after horizon at edgeId " + edge.edge_id);
        }
        if (!edge.released){
            horizon_started = true;
        }
    }
    return std::nullopt;
}

std::optional<OrderManager::Rejection> OrderManager::validate_capabilities(const vda5050::Order& order) const{
    using namespace vda5050;

    auto check_actions = [&](const std::vector<Action>& actions, ActionScope scope,
                            const char* where) -> std::optional<Rejection>{
        if (action_scopes_.empty()){
            return std::nullopt;
        }
        for (const auto& a : actions){
            const auto it = action_scopes_.find(a.action_type);
            if (it == action_scopes_.end() || !it->second.count(scope)){
                const std::string why = it == action_scopes_.end()
                    ? "Unsupported action " + a.action_type
                    : "Action " + a.action_type + " is not allowed on " + where;
                return Rejection{
                    error_type::kInvalidOrderAction, ErrorLevel::Warning, why,
                    {error_ref("actionId", a.action_id), error_ref("actionType", a.action_type)}
                };
            }
        }
        return std::nullopt;
    };

    for (const auto& node : order.nodes){
        if (auto r = check_actions(node.actions, ActionScope::Node, "node")) return r;

        if (node.node_position && State_.maps){
            const auto& maps = *State_.maps;
            const bool enabled = std::any_of(maps.begin(), maps.end(), [&](const Map& m){
                return m.map_id == node.node_position->map_id && m.map_status == EnableStatus::Enabled;
            });
            if (!enabled){
                return Rejection{
                    error_type::kUnknownMapId, ErrorLevel::Warning, "Unknown map " + node.node_position->map_id,
                    {error_ref("nodeId", node.node_id)}
                };
            }
        }
    }
    for (const auto& edge : order.edges){
        if (auto r = check_actions(edge.actions, ActionScope::Edge, "edge")) return r;
    }
    return std::nullopt;
}

bool OrderManager::is_idle() const{
    using vda5050::ActionStatus;
    return State_.node_states.empty() && State_.edge_states.empty() && std::all_of(
        State_.action_states.begin(), State_.action_states.end(), [](const auto& a){
            return a.action_status == ActionStatus::Finished || a.action_status == ActionStatus::Failed;
        });
}

bool OrderManager::is_start_node_in_range(const vda5050::Node& node) const{
    if (!node.node_position){
        return State_.last_node_id.empty() || State_.last_node_id == node.node_id;
    }
    if (!State_.mobile_robot_position || !State_.mobile_robot_position->localized){
        return false;
    }
    const auto& pos = State_.mobile_robot_position.value();
    const auto& np = node.node_position.value();
    if (pos.map_id != np.map_id){
        return false;
    }

    const double dx = pos.x - np.x;
    const double dy = pos.y - np.y;

    if (!np.allowed_deviation_xy){
        return std::hypot(dx, dy) <= 0.5;
    }
    // Переводим смещение в систему координат эллипса.
    const auto& e = *np.allowed_deviation_xy;
    if (e.a <= 0.0 || e.b <= 0.0){
        return std::hypot(dx, dy) <= 1e-3;
    }
    const double c = std::cos(e.theta), s = std::sin(e.theta);
    const double u = c * dx + s * dy;
    const double v = -s * dx + c * dy;
    return (u * u) / (e.a * e.a) + (v * v) / (e.b * e.b) <= 1.0;
}

void OrderManager::append_states(const vda5050::Order& order, std::size_t first_node_index){
    using namespace vda5050;

    auto add_actions = [&](const std::vector<Action>& actions){
        for (const auto& a : actions){
            ActionState st;
            st.action_id = a.action_id;
            st.action_type = a.action_type;
            st.action_descriptor = a.action_descriptor;
            st.action_status = ActionStatus::Waiting;
            State_.action_states.push_back(std::move(st));
        }
    };

    for (std::size_t i = first_node_index; i < order.nodes.size(); ++i){
        const auto& node = order.nodes[i];
        Order_.nodes.push_back(node);
        add_actions(node.actions);
        if (i == 0){
            continue;
        }
        NodeState ns;
        ns.node_id = node.node_id;
        ns.sequence_id = node.sequence_id;
        ns.node_descriptor = node.node_descriptor;
        ns.released = node.released;
        ns.node_position = node.node_position;
        State_.node_states.push_back(std::move(ns));
    }

    for (const auto& edge : order.edges){
        Order_.edges.push_back(edge);
        add_actions(edge.actions);
        EdgeState es;
        es.edge_id = edge.edge_id;
        es.sequence_id = edge.sequence_id;
        es.edge_descriptor = edge.edge_descriptor;
        es.released = edge.released;
        es.trajectory = edge.trajectory;
        State_.edge_states.push_back(std::move(es));
    }
}

bool same_content(const vda5050::Order& a, const vda5050::Order& b){
    return vda5050::json(a.nodes) == vda5050::json(b.nodes) && vda5050::json(a.edges) == vda5050::json(b.edges) && a.
        order_description == b.order_description;
}

const vda5050::Node* OrderManager::decision_point() const{
    const auto it = std::find_if(Order_.nodes.rbegin(), Order_.nodes.rend(),
                                 [](const vda5050::Node& n){ return n.released; });
    return it == Order_.nodes.rend() ? nullptr : &*it;
}

void OrderManager::drop_horizon(){
    std::set<std::string> horizon_action_ids;
    for (const auto& n : Order_.nodes)
        if (!n.released) for (const auto& a : n.actions) horizon_action_ids.insert(a.action_id);
    for (const auto& e : Order_.edges)
        if (!e.released) for (const auto& a : e.actions) horizon_action_ids.insert(a.action_id);

    auto unreleased = [](const auto& x){ return !x.released; };
    Order_.nodes.erase(std::remove_if(Order_.nodes.begin(), Order_.nodes.end(), unreleased), Order_.nodes.end());
    Order_.edges.erase(std::remove_if(Order_.edges.begin(), Order_.edges.end(), unreleased), Order_.edges.end());
    State_.node_states.erase(std::remove_if(State_.node_states.begin(), State_.node_states.end(), unreleased),
                             State_.node_states.end());
    State_.edge_states.erase(std::remove_if(State_.edge_states.begin(), State_.edge_states.end(), unreleased),
                             State_.edge_states.end());
    State_.action_states.erase(std::remove_if(State_.action_states.begin(), State_.action_states.end(),
                                              [&](const auto& a){ return horizon_action_ids.count(a.action_id) > 0; }),
                               State_.action_states.end());
}

void OrderManager::clear_order_errors(){
    auto& errs = State_.errors;
    errs.erase(std::remove_if(errs.begin(), errs.end(), [](const vda5050::Error& e){
        return kOrderRejectionErrors.count(e.error_type) > 0 || kActionWarnings.count(e.error_type) > 0;
    }), errs.end());
}

void OrderManager::update(const std::string& json_order){
    namespace vda = vda5050;
    auto parsed = parse_order(json_order);
    if (!parsed){
        return;
    }

    const vda::Order& incoming = parsed.value();

    if (auto r = validate_structure(incoming)){
        reject(*r, &incoming);
        return;
    }

    if (State_.operating_mode != vda::OperatingMode::Automatic && State_.operating_mode !=
        vda::OperatingMode::Semiautomatic){
        reject({
                   vda::error_type::kMobileRobotNotAvailable, vda::ErrorLevel::Warning,
                   "Operating mode does not allow orders", {}
               }, &incoming);
        return;
    }

    if (auto r = validate_capabilities(incoming)){
        reject(*r, &incoming);
        return;
    }

    const bool is_new_order = incoming.order_id != State_.order_id;

    if (is_new_order){
        if (!is_idle()){
            reject({
                       vda::error_type::kOtherOrderActive, vda::ErrorLevel::Warning,
                       "Another order is active: " + State_.order_id, {error_ref("orderId", incoming.order_id)}
                   }, &incoming);
            return;
        }
        // (4) Новый order всегда начинается с orderUpdateId = 0.
        if (incoming.order_update_id != 0){
            reject({
                       vda::error_type::kValidationFailure, vda::ErrorLevel::Warning,
                       "New order must start with orderUpdateId 0",
                       {error_ref("orderUpdateId", std::to_string(incoming.order_update_id))}
                   }, &incoming);
            return;
        }
        // (5) Первый узел тривиально достижим.
        if (!is_start_node_in_range(incoming.nodes.front())){
            reject({
                       vda::error_type::kStartNodeOutOfRange, vda::ErrorLevel::Warning, "Start node is out of range",
                       {error_ref("nodeId", incoming.nodes.front().node_id)}
                   }, &incoming);
            return;
        }

        State_.node_states.clear();
        State_.edge_states.clear();
        State_.action_states.clear();

        Order_ = vda::Order{};
        Order_.order_id = incoming.order_id;
        order_canceled = false;

        State_.order_id = incoming.order_id;
        State_.order_update_id = incoming.order_update_id;
        // Первый узел не попадает в nodeStates (6.1.1): робот уже на нём.
        State_.last_node_id = incoming.nodes.front().node_id;
        State_.last_node_sequence_id = incoming.nodes.front().sequence_id;

        append_states(incoming, 0);

        spdlog::info("[order_manager][{}] New order ID: {} accepted", State_.header.serial_number, incoming.order_id);
    } else{
        if (incoming.order_update_id < State_.order_update_id){
            reject({
                       vda::error_type::kOutdatedOrderUpdate, vda::ErrorLevel::Warning, "Order update is outdated",
                       {
                           error_ref("orderId", incoming.order_id),
                           error_ref("orderUpdateId", std::to_string(incoming.order_update_id))
                       }
                   }, &incoming);
            return;
        }
        // (7) После cancelOrder update'ы к этому order запрещены.
        if (order_canceled){
            reject({
                       vda::error_type::kOrderUpdateFollowingCancel, vda::ErrorLevel::Warning, "Order was cancelled",
                       {error_ref("orderId", incoming.order_id)}
                   }, &incoming);
            return;
        }
        // (8) Тот же orderUpdateId: повтор — молча игнорируем, другое содержимое — ошибка.
        if (incoming.order_update_id == State_.order_update_id){
            if (same_content(incoming, Last_received_order)){
                spdlog::error("[order_manager][{}]Error: resending the order", State_.header.serial_number);
                return;
            }
            reject({
                       vda::error_type::kSameOrderUpdateId, vda::ErrorLevel::Warning,
                       "Same orderUpdateId with different content",
                       {
                           error_ref("orderId", incoming.order_id),
                           error_ref("orderUpdateId", std::to_string(incoming.order_update_id))
                       }
                   }, &incoming);
            return;
        }

        // (9) / (10) Первый узел update'а — decision point (последний узел base).
        const vda::Node* dp = decision_point();
        const vda::Node& stitch = incoming.nodes.front();
        if (!dp || dp->node_id != stitch.node_id || dp->sequence_id != stitch.sequence_id){
            reject({
                       vda::error_type::kValidationFailure, vda::ErrorLevel::Warning,
                       "Order update does not start at the decision point",
                       {
                           error_ref("nodeId", stitch.node_id),
                           error_ref("sequenceId", std::to_string(stitch.sequence_id))
                       }
                   }, &incoming);
            return;
        }

        // (9) Робот ещё едет или ждёт update: старый horizon заменяется новым.
        // (10) Робот idle: horizon и так пуст.
        drop_horizon();

        State_.order_update_id = incoming.order_update_id;
        // Stitching node уже есть в order — добавляем всё после него.
        append_states(incoming, 1);

        spdlog::info("[order_manager][{}] Order ID: {} updated to ID: ", State_.header.serial_number, incoming.order_id,
                     incoming.order_update_id);
    }

    Last_received_order = incoming;
    clear_order_errors();
}
