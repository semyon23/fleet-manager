
#include "../include/DiffDriveController.hpp"
#include <algorithm>
DiffDriveController::DiffDriveController(double start_x, double start_y, double start_theta, double dt)
    :  x_(start_x), y_(start_y), theta_(start_theta), dt_(dt){};

void DiffDriveController::set_target_position(double x, double y, double theta, bool has_theta)
{
    target_x_ = x; target_y_ = y; target_theta_ = theta; has_target_theta_ = has_theta;
}

void DiffDriveController::update_position()
{
    double dx = target_x_ - x_;
    double dy = target_y_ - y_;
    double distance = std::sqrt(dx * dx + dy * dy);

    double desired_speed = 0.0;
    double desired_omega = 0.0;

    double lin_tol = 0.02;

    if (distance > lin_tol) {
        double target_heading = std::atan2(dy, dx);
        double angle_to_target = normalize_angle(target_heading - theta_);

        if (std::abs(angle_to_target) > M_PI_4) {
            desired_omega = angle_to_target * 3.0;
            desired_speed = 0.0;
        } else {
            desired_speed = distance * 2.0;

            if (desired_speed < 0.15) desired_speed = 0.15;
            if (desired_speed > max_speed_) desired_speed = max_speed_;

            desired_omega = angle_to_target * 3.0;
        }
    }
    else if (has_target_theta_) {
        desired_speed = 0.0;
        double angle_error = normalize_angle(target_theta_ - theta_);

        desired_omega = angle_error * 3.0;

        if (std::abs(desired_omega) < 0.1 && std::abs(angle_error) > 0.01) {
            desired_omega = (angle_error > 0) ? 0.1 : -0.1;
        }
    }

    desired_omega = std::clamp(desired_omega, -max_omega_, max_omega_);
    omega_ = desired_omega;

    vx_ = std::cos(theta_) * desired_speed;
    vy_ = std::sin(theta_) * desired_speed;

    x_ += vx_ * dt_;
    y_ += vy_ * dt_;
    theta_ = normalize_angle(theta_ + omega_ * dt_);
}

void DiffDriveController::stop()
{
    x_ = target_x_;
    y_ = target_y_;
    theta_ = target_theta_;
    vx_ = 0.0;
    vy_ = 0.0;
    omega_ = 0.0;
}

bool DiffDriveController::is_reached(double target_x, double target_y, double target_theta, bool check_theta, double lin_tol) const
{
    double dx = target_x - x_;
    double dy = target_y - y_;
    double distance = std::sqrt(dx * dx + dy * dy);

    if (distance > lin_tol) {
        return false;
    }

    if (check_theta) {
        double angle_error = std::abs(normalize_angle(target_theta - theta_));
        double rot_tolerance = 0.03; // ~1.7 градуса допуск на вращение
        return angle_error <= rot_tolerance;
    }

    return true;
}
