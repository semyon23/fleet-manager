#include "./include/RobotDriveController.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

RobotDriveController::RobotDriveController(const DiffDriveParams& params) : p_(params) {}

void RobotDriveController::set_path(std::vector<DriveWaypoint> wps)
{
    std::lock_guard lock(mtx_);
    path_.assign(wps.begin(), wps.end());
    if (!path_.empty())
        path_.back().stop = true;
}

void RobotDriveController::append_path(std::vector<DriveWaypoint> wps)
{
    if (wps.empty())
        return;

    std::lock_guard lock(mtx_);

    // По VDA 5050 первая вершина order update совпадает с последней вершиной
    // прошлой base. Если робот ещё не доехал до неё, она уже в очереди:
    // обновляем её параметры и не дублируем.
    if (!path_.empty() && path_.back().node_id == wps.front().node_id) {
        path_.back().stop = wps.front().stop;
        path_.back().theta = wps.front().theta;
        wps.erase(wps.begin());
    }
    path_.insert(path_.end(), wps.begin(), wps.end());
    if (!path_.empty())
        path_.back().stop = true;
}

void RobotDriveController::set_target(double x, double y, std::optional<double> theta,
                                     const std::string& node_id)
{
    set_path({DriveWaypoint{x, y, node_id, true, theta}});
}

void RobotDriveController::standstill()
{
    std::lock_guard lock(mtx_);
    path_.clear();
    speed_ = 0.0;
    omega_ = 0.0;
    vx_ = 0.0;
    vy_ = 0.0;
}

bool RobotDriveController::path_finished() const
{
    std::lock_guard lock(mtx_);
    return path_.empty();
}

std::size_t RobotDriveController::remaining_nodes() const
{
    std::lock_guard lock(mtx_);
    return path_.size();
}

void RobotDriveController::set_on_node_reached(NodeReachedCallback cb)
{
    std::lock_guard lock(mtx_);
    on_node_reached_ = std::move(cb);
}

// ---------------------------------------------------------------------------
//  Вспомогательные расчёты (вызываются под mtx_)
// ---------------------------------------------------------------------------

double RobotDriveController::normalize_angle(double a)
{
    constexpr double pi = std::numbers::pi;
    a = std::fmod(a + pi, 2.0 * pi);
    if (a < 0.0)
        a += 2.0 * pi;
    return a - pi;
}

// Длина пути от робота до ближайшей вершины-остановки
double RobotDriveController::remaining_to_stop() const
{
    double d = 0.0;
    double px = x_, py = y_;
    for (const auto& wp : path_) {
        d += std::hypot(wp.x - px, wp.y - py);
        if (wp.stop)
            break;
        px = wp.x;
        py = wp.y;
    }
    return d;
}

// Допустимая скорость прохождения вершины path_[i] с учётом угла поворота
double RobotDriveController::corner_speed(std::size_t i) const
{
    if (i >= path_.size() || path_[i].stop || i + 1 >= path_.size())
        return 0.0;

    const double px = (i == 0) ? x_ : path_[i - 1].x;
    const double py = (i == 0) ? y_ : path_[i - 1].y;
    const auto& a = path_[i];
    const auto& b = path_[i + 1];

    const double h_in = std::atan2(a.y - py, a.x - px);
    const double h_out = std::atan2(b.y - a.y, b.x - a.x);
    const double turn = std::abs(normalize_angle(h_out - h_in));  // 0 — прямо, PI — разворот

    // прямо -> max_speed, 90° -> ~0.7 max_speed, разворот -> min_corner_speed
    return std::max(p_.max_speed * std::cos(turn / 2.0), p_.min_corner_speed);
}

// Проезжаем транзитные вершины: вошли в радиус переключения — берём следующую
void RobotDriveController::pass_transit_nodes(std::vector<std::string>& reached)
{
    while (!path_.empty() && !path_.front().stop) {
        const auto& wp = path_.front();
        const double r = std::max(p_.switch_radius, speed_ * 0.5);  // на скорости раньше
        if (std::hypot(wp.x - x_, wp.y - y_) > r)
            break;
        reached.push_back(wp.node_id);
        path_.pop_front();
    }
}

// ---------------------------------------------------------------------------
//  Шаг симуляции
// ---------------------------------------------------------------------------

void RobotDriveController::update_position(double dt)
{
    // Нулевой или отрицательный шаг (часы, первый тик) — двигать нечего.
    if (dt <= 0.0)
        return;

    std::vector<std::string> reached;
    NodeReachedCallback cb;

    {
        std::lock_guard lock(mtx_);

        pass_transit_nodes(reached);

        double desired_speed = 0.0;
        double desired_omega = 0.0;

        if (!path_.empty()) {
            const auto& wp = path_.front();
            const double dx = wp.x - x_;
            const double dy = wp.y - y_;
            const double dist = std::hypot(dx, dy);
            const double bearing = normalize_angle(std::atan2(dy, dx) - theta_);

            // Точка остановки достигнута: попали в допуск или чуть проскочили её
            const bool at_stop = wp.stop &&
                (dist <= p_.lin_tol ||
                 (dist < 3.0 * p_.lin_tol && std::abs(bearing) > std::numbers::pi / 2.0));

            if (!at_stop) {
                // --- едем к вершине ---
                const double angle_to_target = bearing;

                // а) плавное торможение к ближайшей точке остановки
                const double v_stop = std::sqrt(2.0 * p_.max_accel * remaining_to_stop());
                // б) не влететь в поворот быстрее допустимого
                //    (к моменту переключения, т.е. на границе радиуса switch_radius)
                const double v_corner = corner_speed(0);
                const double r = wp.stop ? 0.0 : std::max(p_.switch_radius, speed_ * 0.5);
                const double d_corner = std::max(0.0, dist - r);
                const double v_turn = std::sqrt(v_corner * v_corner + 2.0 * p_.max_accel * d_corner);

                desired_speed = std::min({p_.max_speed, v_stop, v_turn});

                // в) смотрим не туда — замедляемся, сильно не туда — крутимся на месте
                if (std::abs(angle_to_target) > std::numbers::pi / 2.0)
                    desired_speed = 0.0;
                else
                    desired_speed *= std::cos(angle_to_target);

                // рядом с точкой остановки не пытаемся резко довернуть на неё
                if (wp.stop && dist < 3.0 * p_.lin_tol)
                    desired_omega = 0.0;
                else
                    desired_omega = angle_to_target * p_.k_omega;
            }
            else if (wp.theta) {
                // --- вершина-остановка достигнута, доворачиваем ориентацию ---
                const double angle_error = normalize_angle(*wp.theta - theta_);
                if (std::abs(angle_error) <= p_.ang_tol) {
                    reached.push_back(wp.node_id);
                    path_.pop_front();
                } else {
                    desired_omega = angle_error * p_.k_omega;
                    if (std::abs(desired_omega) < 0.1)
                        desired_omega = (angle_error > 0.0) ? 0.1 : -0.1;
                    // Не перелететь нужный угол за один шаг.
                    desired_omega = std::clamp(desired_omega, -std::abs(angle_error) / dt,
                                               std::abs(angle_error) / dt);
                }
            }
            else {
                // --- вершина-остановка достигнута, ориентация не важна ---
                reached.push_back(wp.node_id);
                path_.pop_front();
            }
        }

        // ограничение ускорения: скорость меняется плавно
        const double max_dv = p_.max_accel * dt;
        speed_ += std::clamp(desired_speed - speed_, -max_dv, max_dv);
        if (path_.empty() && std::abs(speed_) < 1e-3)
            speed_ = 0.0;

        omega_ = std::clamp(desired_omega, -p_.max_omega, p_.max_omega);
        vx_ = std::cos(theta_) * speed_;
        vy_ = std::sin(theta_) * speed_;

        x_ += vx_ * dt;
        y_ += vy_ * dt;
        theta_ = normalize_angle(theta_ + omega_ * dt);

        cb = on_node_reached_;
    }

    // колбэки вызываем без блокировки, чтобы из них можно было звать контроллер
    if (cb)
        for (const auto& id : reached)
            cb(id);
}

// ---------------------------------------------------------------------------
//  Состояние
// ---------------------------------------------------------------------------

void RobotDriveController::set_pose(double x, double y, double theta)
{
    std::lock_guard lock(mtx_);
    x_ = x;
    y_ = y;
    theta_ = normalize_angle(theta);
}

void RobotDriveController::set_params(const DiffDriveParams& params)
{
    std::lock_guard lock(mtx_);
    p_ = params;
}

double RobotDriveController::x() const      { std::lock_guard l(mtx_); return x_; }
double RobotDriveController::y() const      { std::lock_guard l(mtx_); return y_; }
double RobotDriveController::theta() const  { std::lock_guard l(mtx_); return theta_; }
double RobotDriveController::vx() const     { std::lock_guard l(mtx_); return vx_; }
double RobotDriveController::vy() const     { std::lock_guard l(mtx_); return vy_; }
double RobotDriveController::speed() const  { std::lock_guard l(mtx_); return speed_; }
double RobotDriveController::omega() const  { std::lock_guard l(mtx_); return omega_; }

bool RobotDriveController::is_moving() const
{
    std::lock_guard l(mtx_);
    return std::abs(speed_) > 1e-3 || std::abs(omega_) > 1e-3;
}
