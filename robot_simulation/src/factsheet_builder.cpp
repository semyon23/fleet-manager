#include "include/factsheet_builder.hpp"
#include <chrono>

namespace {

namespace v = vda5050;

double seconds(std::chrono::milliseconds ms) {
    return std::chrono::duration<double>(ms).count();
}

v::MobileRobotAction make_action(const std::string& type, const std::string& description,
                                 std::vector<v::ActionScope> scopes,
                                 std::vector<v::BlockingType> blocking = {v::BlockingType::None}) {
    v::MobileRobotAction a;
    a.action_type = type;
    a.action_description = description;
    a.action_scopes = std::move(scopes);
    a.blocking_types = std::move(blocking);
    return a;
}

v::FactsheetActionParameter param(const char* key, v::ValueDataType type, bool optional) {
    v::FactsheetActionParameter p;
    p.key = key;
    p.value_data_type = type;
    p.is_optional = optional;
    return p;
}

v::OptionalParameter supported(const char* name) {
    return {name, v::Support::Supported, std::nullopt};
}

v::TypeSpecification type_specification(const RobotConfig& c) {
    v::TypeSpecification ts;
    ts.series_name = c.physical.series_name;
    ts.series_description = c.physical.series_description;
    ts.mobile_robot_kinematics = c.physical.kinematics;     // значения проверил загрузчик конфига
    ts.mobile_robot_class = c.physical.robot_class;
    ts.maximum_load_mass = c.load_handling.max_load_mass;
    ts.localization_types = {v::localization_type::kNatural};
    ts.navigation_types = {c.physical.navigation_type};
    ts.supported_zones = std::vector<v::ZoneType>{};          // зоны пока не поддерживаются
    return ts;
}

v::PhysicalParameters physical_parameters(const RobotConfig& c) {
    // Скорости и ускорения — из конфига контроллера: fms должен рассчитывать время
    // пути так же, как робот реально едет.
    v::PhysicalParameters p;
    p.minimum_speed = c.motion.min_corner_speed;
    p.maximum_speed = c.motion.max_speed;
    p.maximum_angular_speed = c.motion.max_omega;
    p.maximum_acceleration = c.motion.max_accel;
    p.maximum_deceleration = c.motion.max_accel;
    p.minimum_height = c.physical.min_height;
    p.maximum_height = c.physical.max_height;
    p.width = c.physical.width;
    p.length = c.physical.length;
    return p;
}

v::ProtocolLimits protocol_limits(const RobotConfig& c) {
    v::ProtocolLimits lim;
    lim.maximum_string_lengths.maximum_id_length = 64;
    lim.maximum_string_lengths.id_numerical_only = false;
    lim.maximum_array_lengths.order_nodes = 200;
    lim.maximum_array_lengths.order_edges = 199;
    lim.maximum_array_lengths.node_actions = 10;
    lim.maximum_array_lengths.edge_actions = 10;
    lim.maximum_array_lengths.instant_actions = 10;
    // Те же интервалы, с которыми работает StatePublisher.
    // В vda5050/factsheet.hpp поля Timing — float: в JSON будет 0.10000000149...
    // Чтобы было 0.1, поменяй их тип на double.
    lim.timing.minimum_order_interval = 0.1;
    lim.timing.minimum_state_interval = seconds(c.publisher.min_state_interval);
    lim.timing.default_state_interval = seconds(c.publisher.max_state_interval);
    lim.timing.visualization_interval = seconds(c.publisher.visualization_interval);
    return lim;
}

v::ProtocolFeatures protocol_features(const std::set<std::string>& handlers) {
    using v::ActionScope;
    using v::BlockingType;
    using v::ValueDataType;

    v::ProtocolFeatures f;
    // Optional-поля order'а, которые робот понимает (7.1.1).
    f.optional_parameters = {
        supported("order.nodes.nodePosition.theta"),
        supported("order.nodes.nodePosition.allowedDeviationXY"),
        supported("order.nodes.nodePosition.allowedDeviationTheta"),
        supported("order.edges.maximumSpeed"),
        {"order.nodes.nodePosition", v::Support::Required,
         std::string("node coordinates are needed to drive")},
    };

    auto& actions = f.mobile_robot_actions;

    // Обязательные для любого робота (6.2.3) и служебные — их выполняет сам робот.
    auto cancel = make_action(v::action_type::kCancelOrder, "Cancel the active order",
                              {ActionScope::Instant});
    cancel.action_parameters = std::vector{param("orderId", ValueDataType::String, true)};
    actions.push_back(cancel);
    actions.push_back(make_action(v::action_type::kStartPause, "Pause order execution", {ActionScope::Instant}));
    actions.push_back(make_action(v::action_type::kStopPause, "Resume order execution", {ActionScope::Instant}));
    actions.push_back(make_action(v::action_type::kFactsheetRequest, "Publish factsheet", {ActionScope::Instant}));
    actions.push_back(make_action(v::action_type::kStateRequest, "Publish state", {ActionScope::Instant}));

    // Action'ы из ActionExecutor: только те, для которых есть обработчик.
    const auto load_params = std::vector{param("lhd", ValueDataType::String, true),
                                         param("loadId", ValueDataType::String, true),
                                         param("loadType", ValueDataType::String, true)};
    for (const auto& type : handlers) {
        if (type == v::action_type::kPick || type == v::action_type::kDrop) {
            const bool pick = type == v::action_type::kPick;
            auto a = make_action(type, pick ? "Pick a load at the node" : "Drop the load at the node",
                                 {ActionScope::Node}, {BlockingType::Hard});
            a.action_parameters = load_params;
            a.action_result = pick ? "loadId of the picked load" : "loadId of the dropped load";
            a.pause_allowed = true;
            a.cancel_allowed = false;   // груз в движении — прерывать нельзя
            actions.push_back(std::move(a));
        } else {
            // Свой action без описания параметров: на узле, блокирующий.
            actions.push_back(make_action(type, "Custom action", {ActionScope::Node},
                                          {BlockingType::Hard}));
        }
    }
    return f;
}

v::MobileRobotGeometry geometry(const RobotConfig& c) {
    // Контур робота — прямоугольник по габаритам, центр в начале координат робота.
    const double hx = c.physical.length / 2.0;
    const double hy = c.physical.width / 2.0;
    v::Envelope2d contour;
    contour.envelope2d_id = "mechanical";
    contour.description = "Robot footprint without load";
    contour.vertices = {{hx, hy}, {hx, -hy}, {-hx, -hy}, {-hx, hy}};

    v::MobileRobotGeometry g;
    g.envelopes2d = std::vector{contour};
    return g;
}

v::LoadSpecification load_specification(const RobotConfig& c, bool handles_loads) {
    v::LoadSpecification spec;
    if (!handles_loads) {
        spec.load_positions = std::vector<std::string>{};   // грузозахвата нет
        return spec;
    }
    const auto& l = c.load_handling;
    spec.load_positions = std::vector<std::string>{l.lhd};

    v::LoadSet set;
    set.set_name = "DEFAULT";
    set.load_type = l.load_type;
    set.load_positions = std::vector<std::string>{l.lhd};
    set.maximum_weight = l.max_load_mass;
    set.pick_time = l.pick_time;   // те же значения, что у обработчиков pick/drop
    set.drop_time = l.drop_time;
    spec.load_sets = std::vector{set};
    return spec;
}

}  // namespace

vda5050::Factsheet build_factsheet(const RobotConfig& config,
                                   const std::set<std::string>& supported_actions) {
    v::Factsheet f;
    f.header.version = v::kProtocolVersion;
    f.header.manufacturer = config.identity.manufacturer;
    f.header.serial_number = config.identity.serial_number;

    f.type_specification = type_specification(config);
    f.physical_parameters = physical_parameters(config);
    f.protocol_limits = protocol_limits(config);
    f.protocol_features = protocol_features(supported_actions);
    f.mobile_robot_geometry = geometry(config);

    const bool handles_loads = supported_actions.count(v::action_type::kPick) ||
                               supported_actions.count(v::action_type::kDrop);
    f.load_specification = load_specification(config, handles_loads);

    v::MobileRobotConfiguration rc;
    rc.versions = std::vector<v::VersionInfo>{{"softwareVersion", config.physical.software_version}};
    f.mobile_robot_configuration = rc;
    return f;
}
