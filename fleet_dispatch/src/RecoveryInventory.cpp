#include "../include/RecoveryInventory.hpp"


RecoveryInventory::RecoveryInventory(const std::string &instance) {
    view_.set_instance_id(instance);
    view_.set_revision(1);
}

void RecoveryInventory::reject_startup() {
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
                               bool startup_consistent, Clock::time_point read_started) {
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
        if (!google::protobuf::util::MessageDifferencer::Equals(view_, next)) {
            next.set_revision(view_.revision() + 1);
            view_.Swap(&next);
        }
        expires_ = read_started + std::chrono::seconds(3);
}
