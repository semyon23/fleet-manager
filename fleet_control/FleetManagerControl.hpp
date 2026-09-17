

#ifndef FLEETMANAGERCONTROL_HPP
#define FLEETMANAGERCONTROL_HPP



class FleetManagerControl {
public:
    FleetManagerControl();
    void startup();
    void update_task();
    void update_robot_nodes();
    void submit_navigation_mission();
    void submit_charging_mission();
    void submit_pickplace_mission();
    void submit_undock_mission();
    void submit_obj_detection_mission();
    void submit_action_mission();
    void find_nearest_task_location();
    void shortest_path();
    void create_missions();
    void raise_if_mission_exists();
    void calculate_route();
    void optimize_graph_cpu();
    void get_available_objects();
    void wait_for_mission();
    void wait_for_mission_wrapper();
    void get_route_path_data();
    void visualize_route();
};



#endif //FLEETMANAGERCONTROL_HPP
