#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct DriveWaypoint {
    double x = 0.0;
    double y = 0.0;
    std::string node_id;
    bool stop = false;
    std::optional<double> theta;
};

struct DiffDriveParams {
   // double dt = 0.01;                // [s] шаг симуляции
    double max_speed = 1.0;          // [m/s]
    double max_omega = 1.5;          // [rad/s]
    double max_accel = 0.5;          // [m/s^2] разгон и торможение
    double switch_radius = 0.25;     // [m] радиус переключения на следующую вершину
    double min_corner_speed = 0.1;   // [m/s] скорость в самом крутом повороте
    double lin_tol = 0.02;           // [m] точность в вершине-остановке
    double ang_tol = 0.01;           // [rad] точность ориентации
    double k_omega = 3.0;            // коэффициент регулятора курса
};

class RobotDriveController {
public:
    using NodeReachedCallback = std::function<void(const std::string& node_id)>;

    RobotDriveController() = default;
    explicit RobotDriveController(const DiffDriveParams& params);

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

    const DiffDriveParams& params() const { return p_; }
    void set_params(const DiffDriveParams& params);

    static double normalize_angle(double a);

private:

    double remaining_to_stop() const;
    double corner_speed(std::size_t i) const;
    void pass_transit_nodes(std::vector<std::string>& reached);

    DiffDriveParams p_;

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
