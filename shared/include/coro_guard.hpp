#ifndef FLEETMANAGERSYSTEM_CORO_GUARD_HPP
#define FLEETMANAGERSYSTEM_CORO_GUARD_HPP

#include <functional>

class CoroGuard {
public:
    CoroGuard(std::function<void()> cleanup = nullptr)
        : cleanup_(std::move(cleanup)) {}
    ~CoroGuard() {
        if (cleanup_) {
            try {
                cleanup_();
            } catch (const std::exception& e) {
                spdlog::error("[CoroGuard]Exception in cleanup callback: ", e.what());
            }
        }
    }

    CoroGuard(const CoroGuard&) = delete;
    CoroGuard& operator=(const CoroGuard&) = delete;

private:
    std::function<void()> cleanup_;
};
#endif //FLEETMANAGERSYSTEM_CORO_GUARD_HPP
