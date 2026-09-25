//
// Created by ubuntu on 9/23/26.
//

#ifndef FLEETMANAGERSYSTEM_DIFFDRIVECONTROLLER_HPP
#define FLEETMANAGERSYSTEM_DIFFDRIVECONTROLLER_HPP

#pragma once
#include <cmath>

class DiffDriveController {
public:
    DiffDriveController(double start_x = 0.0, double start_y = 0.0, double start_theta = 0.0, double dt = 0.1);
    void set_target_position(double x, double y, double theta, bool has_theta);
    void update_position();
    void stop();
    bool is_reached(double target_x, double target_y, double target_theta, bool check_theta, double lin_tol) const;
    double x() const { return x_; }
    double y() const { return y_; }
    double theta() const { return theta_; }

    double vx() const { return vx_; }
    double vy() const { return vy_; }
    double omega() const { return omega_; }
private:
    double x_, y_, theta_, dt_;
    double target_x_ = 0.0, target_y_ = 0.0, target_theta_ = 0.0;
    bool has_target_theta_ = false;

    const double max_speed_ = 1.0;
    const double max_omega_ = 1.5;

    double vx_ = 0.0;
    double vy_ = 0.0;
    double omega_ = 0.0;

    double normalize_angle(double angle) const {
        while (angle > M_PI)  angle -= 2.0 * M_PI;
        while (angle < -M_PI) angle += 2.0 * M_PI;
        return angle;
    }
};


// class DiffDriveController {
// public:
//     DiffDriveController(double start_x = 0.0, double start_y = 0.0, double start_theta = 0.0);
//     void set_target(double x, double y, double theta, bool has_theta);
//
//     bool is_reached(double target_x, double target_y, double target_theta, bool check_theta, double lin_tol) const;
//     void update_step(double dt);
//     void stop();
//
// private:
//     double x_, y_, theta_;
//     double target_x_ = 0.0, target_y_ = 0.0, target_theta_ = 0.0;
//     bool has_target_theta_ = false;
//     bool standstill_ = true;
//
//     const double max_speed_ = 1.0;
//     const double max_omega_ = 1.5;
//
//     double normalize_angle(double angle) const {
//         while (angle > M_PI)  angle -= 2.0 * M_PI;
//         while (angle < -M_PI) angle += 2.0 * M_PI;
//         return angle;
//     }
// };


#endif //FLEETMANAGERSYSTEM_DIFFDRIVECONTROLLER_HPP
