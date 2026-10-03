

#include "include/RobotSimulator.hpp"
#include <spdlog/spdlog.h>
namespace {

namespace v = vda5050;

v::MobileRobotAction make_action(const char* type, const char* description,
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

}  // namespace

void RobotSimulator::init_factsheet() {
    using v::ActionScope;
    using v::BlockingType;
    using v::ValueDataType;

    Factsheet = v::Factsheet{};
    Factsheet.header.version = v::kProtocolVersion;
    Factsheet.header.manufacturer = config.manufacturer;
    Factsheet.header.serial_number = config.name;

    // --- typeSpecification: тип (серия) робота, а не конкретный экземпляр ---
    auto& ts = Factsheet.type_specification;
    ts.series_name = "MecanumBot";  // одинаково у всех роботов этой модели; экземпляр — serialNumber
    ts.series_description = "Simulated mecanum carrier";
    ts.mobile_robot_kinematics = v::mobile_robot_kinematics::kOmnidirectional;
    ts.mobile_robot_class = v::mobile_robot_class::kCarrier;
    ts.maximum_load_mass = 500.0;                                     // [kg]
    ts.localization_types = {v::localization_type::kNatural};
    // Контроллер симулятора едет между узлами по прямой — виртуальные «рельсы».
    ts.navigation_types = {v::navigation_type::kVirtualLineGuided};
    ts.supported_zones = std::vector<v::ZoneType>{};                  // зоны пока не поддерживаются

    // --- physicalParameters ---
    auto& p = Factsheet.physical_parameters;
    p.minimum_speed = 0.01;          // [m/s]
    p.maximum_speed = 1.0;           // [m/s]  — как max_speed контроллера
    p.minimum_angular_speed = 0.05;  // [rad/s]
    p.maximum_angular_speed = 1.0;   // [rad/s]
    p.maximum_acceleration = 0.5;    // [m/s^2] — как max_accel контроллера
    p.maximum_deceleration = 0.5;    // [m/s^2]
    p.minimum_height = 0.25;         // [m]
    p.maximum_height = 0.40;         // [m] с поднятым грузом
    p.width = 0.60;                  // [m]
    p.length = 0.80;                 // [m]

    // --- protocolLimits ---
    auto& lim = Factsheet.protocol_limits;
    lim.maximum_string_lengths.maximum_id_length = 64;
    lim.maximum_string_lengths.id_numerical_only = false;
    lim.maximum_array_lengths.order_nodes = 200;
    lim.maximum_array_lengths.order_edges = 199;
    lim.maximum_array_lengths.node_actions = 10;
    lim.maximum_array_lengths.edge_actions = 10;
    lim.maximum_array_lengths.instant_actions = 10;
    // Должно совпадать с тем, как реально работает отправитель state.
    // [s]. В vda5050/factsheet.hpp поля Timing — float: в JSON получится 0.10000000149...
    // Чтобы было 0.1, поменяй их тип на double.
    lim.timing.minimum_order_interval = 0.1;
    lim.timing.minimum_state_interval = std::chrono::duration<double>(min_state_interval_).count();
    lim.timing.default_state_interval = 30.0;
    lim.timing.visualization_interval = 0.2;

    // --- protocolFeatures: optional-поля order'а, которые робот понимает (7.1.1) ---
    Factsheet.protocol_features.optional_parameters = {
        supported("order.nodes.nodePosition.theta"),
        supported("order.nodes.nodePosition.allowedDeviationXY"),
        supported("order.nodes.nodePosition.allowedDeviationTheta"),
        supported("order.edges.maximumSpeed"),
        {"order.nodes.nodePosition", v::Support::Required,
         std::string("node coordinates are needed to drive")},
    };

    // --- protocolFeatures: action'ы. Только то, что робот реально выполняет. ---
    auto& actions = Factsheet.protocol_features.mobile_robot_actions;
    actions.clear();

    // Обязательные для любого робота (6.2.3).
    auto cancel = make_action(v::action_type::kCancelOrder, "Cancel the active order",
                              {ActionScope::Instant});
    cancel.action_parameters = std::vector{param("orderId", ValueDataType::String, true)};
    actions.push_back(cancel);
    actions.push_back(make_action(v::action_type::kStartPause, "Pause order execution",
                                  {ActionScope::Instant}));
    actions.push_back(make_action(v::action_type::kStopPause, "Resume order execution",
                                  {ActionScope::Instant}));

    // Служебные instant action'ы, которые робот выполняет сам.
    actions.push_back(make_action(v::action_type::kFactsheetRequest, "Publish factsheet",
                                  {ActionScope::Instant}));
    actions.push_back(make_action(v::action_type::kStateRequest, "Publish state",
                                  {ActionScope::Instant}));

    // Action'ы из ActionExecutor: описываем только те, для которых есть обработчик.
    //const auto handlers = actions_.supported_types();
    const std::set<std::string> handlers;
    auto load_params = std::vector{param("lhd", ValueDataType::String, true),
                                   param("loadId", ValueDataType::String, true),
                                   param("loadType", ValueDataType::String, true)};
    if (handlers.count(v::action_type::kPick)) {
        auto pick = make_action(v::action_type::kPick, "Pick a load at the node",
                                {ActionScope::Node}, {BlockingType::Hard});
        pick.action_parameters = load_params;
        pick.action_result = "loadId of the picked load";
        pick.pause_allowed = true;
        pick.cancel_allowed = false;  // груз уже в движении — прерывать нельзя
        actions.push_back(pick);
    }
    if (handlers.count(v::action_type::kDrop)) {
        auto drop = make_action(v::action_type::kDrop, "Drop the load at the node",
                                {ActionScope::Node}, {BlockingType::Hard});
        drop.action_parameters = load_params;
        drop.action_result = "loadId of the dropped load";
        drop.pause_allowed = true;
        drop.cancel_allowed = false;
        actions.push_back(drop);
    }

    // --- mobileRobotGeometry: колёса и контур ---
    const double wx = 0.30, wy = 0.25;  // [m] от центра робота
    std::vector<v::WheelDefinition> wheels;
    for (auto [x, y] : {std::pair{wx, wy}, {wx, -wy}, {-wx, wy}, {-wx, -wy}}) {
        v::WheelDefinition w;
        w.type = v::wheel_type::kMecanum;
        w.is_active_driven = true;
        w.is_active_steered = false;
        w.position = {x, y, std::nullopt};
        w.diameter = 0.15;
        w.width = 0.08;
        wheels.push_back(w);
    }
    Factsheet.mobile_robot_geometry.wheel_definitions = std::move(wheels);
    v::Envelope2d contour;
    contour.envelope2d_id = "mechanical";
    contour.description = "Robot footprint without load";
    contour.vertices = {{0.40, 0.30}, {0.40, -0.30}, {-0.40, -0.30}, {-0.40, 0.30}};
    Factsheet.mobile_robot_geometry.envelopes2d = std::vector{contour};

    // --- loadSpecification: грузозахват и типы грузов ---
    Factsheet.load_specification.load_positions = std::vector<std::string>{"LHD1"};
    v::LoadSet pallet;
    pallet.set_name = "DEFAULT";
    pallet.load_type = "EPAL";
    pallet.load_positions = std::vector<std::string>{"LHD1"};
    pallet.maximum_weight = ts.maximum_load_mass;
    // pallet.pick_time = load_cfg_.pick_time;
    // pallet.drop_time = load_cfg_.drop_time;
    pallet.pick_time = 3.0;
    pallet.drop_time = 4.0;
    Factsheet.load_specification.load_sets = std::vector{pallet};

    // --- mobileRobotConfiguration ---
    v::MobileRobotConfiguration cfg;
    cfg.versions = std::vector<v::VersionInfo>{{"softwareVersion", "0.1.0"}};
    v::BatteryCharging charging;
    charging.critical_low_charging_level = 10.0;
    charging.minimum_desired_charging_level = 30.0;
    charging.maximum_desired_charging_level = 90.0;
    cfg.battery_charging = charging;
    Factsheet.mobile_robot_configuration = cfg;

    spdlog::info("[{}] Robot Factsheet initialized", get_name());
}

