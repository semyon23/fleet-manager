#ifndef FLEETMANAGERSYSTEM_STARTUPINVENTORY_HPP
#define FLEETMANAGERSYSTEM_STARTUPINVENTORY_HPP


#pragma once

#include "fleet_database.pb.h"
#include "fleet_dispatch.pb.h"
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

class StartupInventory {
    using Snapshot = fms::database::v1::DispatchSnapshot;

public:
    explicit StartupInventory(Snapshot data) : snapshot(std::move(data)) {
        namespace db = fms::database::v1;
        std::set<std::pair<std::string, std::string>> identities;
        std::set<std::string> robots;

        for (const auto& r : snapshot.robots()) {
            const auto& s = r.spec();
            if (s.robot_id().empty() || s.manufacturer().empty() || s.serial_number().empty() ||
                !robots.insert(s.robot_id()).second ||
                !identities.emplace(s.manufacturer(), s.serial_number()).second ||
                (r.has_status() && r.status().robot_id() != s.robot_id()) ||
                (s.lifecycle() != db::ROBOT_LIFECYCLE_ACTIVE &&
                 s.lifecycle() != db::ROBOT_LIFECYCLE_PENDING_DELETE)) {
                throw std::invalid_argument("invalid dispatch robot inventory");
            }

            if (s.lifecycle() == db::ROBOT_LIFECYCLE_PENDING_DELETE)
                deleting.insert(s.robot_id());
        }

        std::map<std::string, const db::MissionExecution*> grants;
        for (const auto& g : snapshot.last_grants()) {
            if (!grants.emplace(g.mission().mission_id(), &g).second)
                hold("duplicate last grant");
        }

        auto remember = [&](const std::string& mid, const std::string& rid) {
            if (mid.empty() || !robots.contains(rid)) hold("missing mission owner");
            if (!missions.insert(mid).second) hold("duplicate mission");
            if (!rid.empty() && !occupied.emplace(rid, mid).second)
                hold("multiple executions on one robot");
        };

        for (const auto& e : snapshot.active_missions()) {
            const auto& m = e.mission();
            remember(m.mission_id(), m.robot_id());

            auto it = grants.find(m.mission_id());
            if (it != grants.end()) {
                const auto& g = *it->second;
                if (!g.has_order() || g.mission().robot_id() != m.robot_id() ||
                    g.checkpoint_version() > e.checkpoint_version())
                    hold("grant mismatch");
                grants.erase(it);
            } else {
                for (const auto& n : m.nodes()) {
                    if (n.id() == e.status().current_node_id() && (n.has_move() || n.has_action()))
                        hold("missing last grant");
                }
            }

            if (!e.checkpoint_version() || m.mission_id() != e.status().mission_id() ||
                m.robot_id() != e.status().robot_id())
                hold("checkpoint mismatch");
        }

        for (const auto& u : snapshot.unresolved()) {
            auto rid = u.robot_id();
            if (!u.add_request().empty()) {
                fms::dispatch::v1::AddMissionRequest add;
                if (!add.ParseFromString(u.add_request()) ||
                    add.mission().mission_id() != u.mission_id() ||
                    (!rid.empty() && rid != add.mission().robot_id())) {
                    hold("unresolved Add identity");
                } else {
                    rid = add.mission().robot_id();
                }
            }
            remember(u.mission_id(), rid);
            grants.erase(u.mission_id());
        }

        if (!grants.empty()) hold("orphan last grant");
        if (reason.empty() && !missions.empty()) hold("mission reconciliation pending");
    }

    fms::database::v1::DispatchSnapshot snapshot;
    std::map<std::string, std::string> occupied;
    std::set<std::string> missions, deleting;
    std::string reason;

private:
    void hold(const char* text) {
        if (reason.empty()) reason = text;
    }
};

#endif //FLEETMANAGERSYSTEM_STARTUPINVENTORY_HPP
