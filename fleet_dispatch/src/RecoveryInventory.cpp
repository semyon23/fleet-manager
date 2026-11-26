#include "../include/RecoveryInventory.hpp"
#include <ranges>


RecoveryInventory::RecoveryInventory(const std::string &instance) {
    view_.set_instance_id(instance);
    view_.set_revision(1);
}

namespace {
using View = fms::dispatch::v1::GetRecoveryInventoryResponse;
using Permit = RecoveryInventory::MotionPermit;

// A claim and an execution write legitimately change this robot's compact row.
// Every other robot and reservation owner must still match the admitted view.
bool own_write_only(const View& before, const View& next, const Permit& p) {
    const auto& id = p.scope.robot_id();
    if (next.state() != View::READY || !std::ranges::equal(before.robots(), next.robots())) return false;
    const auto others = std::views::filter([&](const std::string& robot) { return robot != id; });
    const auto other_missions = std::views::filter([&](const auto& m) { return m.robot_id() != id; });
    if (!std::ranges::equal(before.blocked_robots() | others, next.blocked_robots() | others) ||
        !std::ranges::equal(before.live_restrictions().robots() | others, next.live_restrictions().robots() | others) ||
        !std::ranges::equal(before.missions() | other_missions, next.missions() | other_missions,
            [](const auto& a, const auto& b) { return google::protobuf::util::MessageDifferencer::Equals(a, b); }))
        return false;
    for (const auto& m : next.missions()) {
        if (m.robot_id() == id)
            return m.mission_id() == p.mission && m.has_add_attempt() && m.add_attempt() == p.scope.add_attempt() &&
                   (!p.extending || !m.uncertain());
    }
    return !p.extending && std::ranges::none_of(before.missions(), [&](const auto& m) { return m.robot_id() == id; });
}
}

void RecoveryInventory::read_failed() {
    expires_.reset();
    for (const auto& weak : writers_) if (auto p = weak.lock()) p->expires = Clock::time_point::min();
    writers_.clear();
}

std::shared_ptr<const RecoveryInventory::MotionPermit> RecoveryInventory::acquire(
    const fms::dispatch::v1::MotionScope& scope, const std::string& mission, bool extending, Clock::time_point now) {
    if (!expires_ || now >= *expires_ || view_.state() != Reply::READY || !view_.has_live_restrictions() ||
        scope.instance_id().empty() || scope.instance_id() != view_.instance_id() || !scope.revision() ||
        scope.revision() != view_.revision() || !scope.has_add_attempt() || mission.empty()) return {};
    const auto& robot = scope.robot_id();
    const auto contains = [&](const auto& ids) { return std::binary_search(ids.begin(), ids.end(), robot); };
    if (robot.empty() || !contains(view_.robots()) || contains(view_.live_restrictions().robots()) ||
        contains(view_.blocked_robots()) != extending) return {};
    const auto& rows = view_.missions();
    const auto row = std::lower_bound(rows.begin(), rows.end(), mission,
        [](const auto& m, const auto& mid) { return m.mission_id() < mid; });
    const bool present = row != rows.end() && row->mission_id() == mission;
    if (present != extending || (present && (row->robot_id() != robot || row->uncertain() ||
        !row->has_add_attempt() || row->add_attempt() != scope.add_attempt()))) return {};

    std::erase_if(writers_, [](const auto& w) { return w.expired(); });
    for (const auto& w : writers_)
        if (auto p = w.lock(); p && p->scope.robot_id() == robot) return {};
    auto p = std::make_shared<MotionPermit>(MotionPermit{scope, mission, extending, *expires_});
    writers_.push_back(p);
    return p;
}

void RecoveryInventory::reject_startup() {
    read_failed();
    if (view_.state() != Reply::CONFLICT) {
        view_.set_state(Reply::CONFLICT);
        view_.set_revision(view_.revision() + 1);
    }
}

bool RecoveryInventory::identifies_active(const fms::dispatch::v1::GetMissionRequest& request,
                                         Clock::time_point now) const {
    if (!expires_ || now >= *expires_ || view_.state() != Reply::READY ||
        request.expected_instance_id().empty() || request.expected_instance_id() != view_.instance_id() ||
        request.expected_robot_id().empty() || request.mission_id().empty() || !request.has_expected_add_attempt())
        return false;
    const auto& restricted = view_.live_restrictions().robots();
    if (std::binary_search(restricted.begin(), restricted.end(), request.expected_robot_id())) return false;
    const auto& rows = view_.missions();
    const auto row = std::lower_bound(rows.begin(), rows.end(), request.mission_id(),
        [](const auto& entry, const auto& id) { return entry.mission_id() < id; });
    return row != rows.end() && row->mission_id() == request.mission_id() &&
           row->robot_id() == request.expected_robot_id() && !row->uncertain() &&
           row->has_add_attempt() && row->add_attempt() == request.expected_add_attempt();
}

void RecoveryInventory::update(const Rows &rows, const std::map<std::string, RecoveryRobotView> &live,
                               bool startup_consistent, Clock::time_point read_started, bool robot_epoch_changed) {
     Reply next;
        next.set_instance_id(view_.instance_id());
        next.set_revision(view_.revision());
        std::set<std::string> robots(rows.robots().begin(), rows.robots().end());
        bool conflict = !startup_consistent || robots.contains("") ||
            robots.size() != static_cast<std::size_t>(rows.robots_size()) ||
            live.size() != robots.size();

        std::set<std::string> blocked(rows.deleting().begin(), rows.deleting().end());
        auto restricted = blocked;
        for (const auto& id : blocked)
            if (!robots.contains(id)) conflict = true;

        std::map<std::string, std::string> owners;
        std::set<std::string> missions;
        for (const auto& row : rows.missions()) {
            auto* m = next.add_missions();
            m->set_mission_id(row.mission_id());
            m->set_robot_id(row.robot_id());
            if (row.has_add_attempt() && !row.conflict()) m->set_add_attempt(row.add_attempt());
            m->set_uncertain(row.uncertain() || row.conflict());
            if (!row.robot_id().empty()) blocked.insert(row.robot_id());
            if (!owners.emplace(row.robot_id(), row.mission_id()).second ||
                !missions.insert(row.mission_id()).second || row.mission_id().empty() ||
                !robots.contains(row.robot_id()) || row.conflict()) conflict = true;
        }
        auto* entries = next.mutable_missions();
        std::sort(entries->pointer_begin(), entries->pointer_end(), [](const auto* a, const auto* b) {
            return a->mission_id() < b->mission_id();
        });

        for (const auto& id : robots) {
            next.add_robots(id);
            const auto it = live.find(id);
            if (it == live.end()) {
                blocked.insert(id);
                restricted.insert(id);
                conflict = true;
                continue;
            }
            const auto& r = it->second;
            const auto owner = owners.find(id);
            if (r.blocked || r.conflict || (owner != owners.end() && r.mission != owner->second))
                restricted.insert(id);
            if (r.blocked || r.conflict || !r.mission.empty()) blocked.insert(id);
            if (r.conflict || (!r.mission.empty() &&
                (owner == owners.end() || owner->second != r.mission))) conflict = true;
        }
        for (const auto& id : blocked) next.add_blocked_robots(id);
        auto* restrictions = next.mutable_live_restrictions();
        for (const auto& id : restricted) restrictions->add_robots(id);
        next.set_state(conflict ? Reply::CONFLICT : Reply::READY);
        if (robot_epoch_changed || !google::protobuf::util::MessageDifferencer::Equals(view_, next)) {
            std::erase_if(writers_, [&](const auto& w) {
                const auto p = w.lock();
                if (!p) return true;
                if (!own_write_only(view_, next, *p)) p->expires = Clock::time_point::min();
                return false;
            });
            next.set_revision(view_.revision() + 1);
            view_.Swap(&next);
        }
        expires_ = read_started + std::chrono::seconds(3);
}
