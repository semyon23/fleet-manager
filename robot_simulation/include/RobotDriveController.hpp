#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "include/robot_config.hpp"
struct DriveWaypoint {
    double x = 0.0;
    double y = 0.0;
    std::string node_id;
    bool stop = false;
    std::optional<double> theta;
};

class RobotDriveController {
public:
    using NodeReachedCallback = std::function<void(const std::string& node_id)>;

    RobotDriveController() = default;
    explicit RobotDriveController(const MotionConfig config);

    void set_path(std::vector<DriveWaypoint> wps);
    void append_path(std::vector<DriveWaypoint> wps);   // order update: дописать base
    void set_target(double x, double y,            // одна точка (совместимость)
                    std::optional<double> theta = std::nullopt,
                    const std::string& node_id = "");
    void standstill();                             // экстренная остановка, путь очищается

    bool path_finished() const;
    std::size_t remaining_nodes() const;

    void set_on_node_reached(NodeReachedCallback cb);

    // --- Шаг симуляции ---
    void update_position(double dt);

    // --- Состояние ---
    void set_pose(double x, double y, double theta);
    double x() const;
    double y() const;
    double theta() const;
    double vx() const;
    double vy() const;
    double speed() const;
    double omega() const;
    bool is_moving() const;

    // const DiffDriveParams& params() const { return p_; }
    // void set_params(const DiffDriveParams& params);

    static double normalize_angle(double a);

private:

    double remaining_to_stop() const;
    double corner_speed(std::size_t i) const;
    void pass_transit_nodes(std::vector<std::string>& reached);

    MotionConfig config_;

    double x_ = 0.0;
    double y_ = 0.0;
    double theta_ = 0.0;
    double speed_ = 0.0;
    double omega_ = 0.0;
    double vx_ = 0.0;
    double vy_ = 0.0;

    std::deque<DriveWaypoint> path_;
    NodeReachedCallback on_node_reached_;

    // mutable std::mutex mtx_;
};
