#include "missions/MissionRunner.hpp"
#include "missions/ExecutionRecovery.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>

namespace db = fms::database::v1;
namespace dp = fms::dispatch::v1;
namespace types = fms::v1;
using Storage = db::ControlStorage::Stub;
using Dispatch = dp::FleetDispatch::Stub;
using namespace std::chrono_literals;

namespace {
bool cancel_before_send(const Mission& m, const db::ControlCommandRecord& c, std::uint32_t attempt) {
    if (c.kind() != db::CONTROL_COMMAND_KIND_CANCEL_MISSION || c.attempt() != attempt ||
        c.robot_id() != m.robot_id || c.map_id() != m.map_id || c.recovery_version() != 1) return false;
    dp::CancelMissionRequest req;
    if (!req.ParseFromString(c.request_proto()) || req.mission_id() != m.id) return false;
    const auto doc = nlohmann::json::parse(c.recovery_json(), nullptr, false);
    if (!doc.is_object() || !doc.contains("cancelBeforeAdd") || doc["cancelBeforeAdd"] != true ||
        !doc.contains("attempt") || doc["attempt"] != attempt ||
        !doc.contains("mission") || !doc["mission"].is_object() || doc["mission"].value("id", "") != m.id ||
        !doc.contains("protectedBase") || !doc["protectedBase"].is_object()) return false;
    const auto& base = doc["protectedBase"];
    return base.value("key", "") == m.id + "/" + std::to_string(attempt) + "/s0" &&
           base.value("robot", "") == m.robot_id && base.value("map", "") == m.map_id &&
           base.contains("version") && base["version"] == c.map_version() &&
           base.contains("nodes") && base["nodes"].is_array() && base["nodes"].size() == 1 &&
           base.contains("edges") && base["edges"].is_array() && base["edges"].empty() &&
           base.contains("reached") && base["reached"] == 0;
}

bool newer_than(const types::MissionStatus& a, const types::MissionStatus& b) {
    if (a.updated_at().seconds() != b.updated_at().seconds())
        return a.updated_at().seconds() > b.updated_at().seconds();
    return a.updated_at().nanos() > b.updated_at().nanos();
}
} // namespace

struct MissionRunner::PendingHandoff {
    std::uint64_t epoch = 0;
    execution_recovery::Decision evidence;
    db::CloseMissionCommandsRequest close;
    std::vector<std::string> owned_keys;
};

void MissionRunner::queue_checkpoint(const std::string& id) {
    auto [it, inserted] = checkpoint_jobs_.try_emplace(id);
    if (inserted) {
        it->second = std::make_shared<CheckpointJob>();
        checkpoint_timer_.cancel_one();
    }
}

bool MissionRunner::checkpoint_current(const std::string& id, const Job& job, std::uint64_t epoch) const {
    const auto it = checkpoint_jobs_.find(id);
    return !stopped_ && it != checkpoint_jobs_.end() && it->second == job && job->epoch == epoch;
}

asio::awaitable<void> MissionRunner::checkpoint_loop() {
    constexpr std::size_t limit = 8;
    std::string cursor;
    while (!stopped_) {
        if (journal_loaded_ && checkpoint_inflight_ < limit && !checkpoint_jobs_.empty()) {
            std::vector<std::pair<std::string, Job>> batch;
            auto it = checkpoint_jobs_.upper_bound(cursor);
            const auto now = std::chrono::steady_clock::now();
            for (auto left = checkpoint_jobs_.size(); left && checkpoint_inflight_ + batch.size() < limit; --left) {
                if (it == checkpoint_jobs_.end()) it = checkpoint_jobs_.begin();
                if (!it->second->busy && it->second->due <= now) batch.push_back(*it);
                ++it;
            }
            for (const auto& [id, job] : batch) {
                cursor = id;
                job->busy = true;
                ++checkpoint_inflight_;
                asio::co_spawn(io_context_, recover_checkpoint(id, job, job->epoch),
                    [this, id, job](std::exception_ptr error) {
                        --checkpoint_inflight_;
                        job->busy = false;
                        job->due = std::chrono::steady_clock::now() + job->delay;
                        job->delay = std::min(job->delay * 2, 4000ms);
                        if (error) {
                            try { std::rethrow_exception(error); }
                            catch (const std::exception& ex) {
                                spdlog::error("[recovery] mission {}: {}", id, ex.what());
                            }
                        }
                        checkpoint_timer_.cancel_one();
                    });
            }
        }
        checkpoint_timer_.expires_after(checkpoint_jobs_.empty() ? 24h : 200ms);
        co_await checkpoint_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
}

asio::awaitable<void> MissionRunner::recover_checkpoint(std::string id, Job job, std::uint64_t epoch) {
    const auto current = [&] { return checkpoint_current(id, job, epoch); };
    if (!current() || sending_.contains(id)) co_return;
    if (job->handoff && job->handoff->epoch == epoch) {
        co_await handoff_checkpoint(id, job, epoch);
        co_return;
    }

    db::ListControlCommandsRequest list;
    list.set_mission_id(id);
    list.set_include_resolved(true);
    db::ListControlCommandsResponse rows;
    auto status = co_await services_.call<&Storage::PrepareAsyncListControlCommands>(*services_.control, list, rows);
    if (!current() || !status.ok() || rows.commands().empty()) co_return;
    RecoveryCommands commands;
    std::uint32_t attempt = 0;
    for (const auto& row : rows.commands()) {
        if (row.command().mission_id() != id) co_return;
        commands.push_back(row.command());
        attempt = std::max(attempt, row.command().attempt());
    }
    if (journal_attempt(id) != attempt) co_return;

    db::GetMissionRequest get;
    get.set_mission_id(id);
    db::GetMissionResponse durable;
    status = co_await services_.call<&Storage::PrepareAsyncGetMission>(*services_.control, get, durable);
    if (!current() || !status.ok()) co_return;
    const auto& record = durable.mission();
    auto doc = mission_from_json(nlohmann::json::parse(record.body_json()));
    if (doc.id != id || doc.robot_id.empty() || record.mission_id() != id ||
        record.robot_id() != doc.robot_id || record.map_id() != doc.map_id || record.state() != doc.state)
        co_return;

    const auto local_cancel = std::ranges::find_if(commands, [&](const auto& cmd) {
        return cancel_before_send(doc, cmd, attempt);
    });
    if (local_cancel != commands.end()) {
        if (add_rpc_started_.contains(id) || std::ranges::any_of(commands, [](const auto& c) {
            return c.kind() == db::CONTROL_COMMAND_KIND_EXTEND_MISSION;
        })) co_return;
        if (auto* active = missions_.find(id)) {
            if (active->robot_id == doc.robot_id)
                finish(*active, "CANCELED", std::nullopt, "canceled before dispatch send");
            co_return;
        }
        if (doc.state != "CANCELED") co_return;
        // Close the proof last so a crash between two Resolve calls is recoverable.
        std::stable_partition(commands.begin(), commands.end(), [&](const auto& c) {
            return c.kind() != db::CONTROL_COMMAND_KIND_CANCEL_MISSION || c.attempt() != attempt;
        });
        for (const auto& c : commands) {
            db::ResolveControlCommandRequest resolve;
            resolve.set_command_id(c.command_id());
            db::ResolveControlCommandResponse ack;
            status = co_await services_.call<&Storage::PrepareAsyncResolveControlCommand>(*services_.control, resolve, ack);
            if (!current() || !status.ok()) co_return;
        }
        journal_attempts_.erase(id);
        checkpoint_jobs_.erase(id);
        co_return;
    }

    const auto* graph = maps_.find(doc.map_id);
    if (!graph) co_return;
    auto auth = execution_recovery::read_authority(doc, *graph, commands, attempt);
    if (!auth) co_return;
    // Reconstruct the locally issued base even when dispatch cannot answer.
    // Across a step transition all possible steps are still durable and motion stays gated.
    const auto known_step = doc.current_step.value_or(0);
    if (known_step < 0 || static_cast<std::size_t>(known_step) >= auth->steps.size()) co_return;
    if (!traffic_routes_.contains(doc.robot_id)) {
        const auto& base = auth->steps[known_step].base;
        traffic_routes_.emplace(doc.robot_id, base);
    }

    types::GetExecutionCheckpointRequest request;
    request.set_mission_id(id);
    request.set_include_route_proof(true);
    types::GetExecutionCheckpointResponse cp;
    status = co_await services_.call<&Dispatch::PrepareAsyncGetExecutionCheckpoint>(*services_.dispatch, request, cp);
    if (!current() || !status.ok() || !cp.has_add_attempt() || cp.add_attempt() != attempt ||
        cp.mission_id() != id || cp.robot_id() != doc.robot_id || !cp.has_status() ||
        cp.status().robot_id() != doc.robot_id || cp.status().mission_id() != id ||
        !cp.checkpoint_version() || cp.checkpoint_version() < job->version) co_return;
    job->version = cp.checkpoint_version();
    graph = maps_.find(doc.map_id);
    if (!graph || graph->version != auth->steps.front().base.version) co_return;
    if (cp.kind() != types::GetExecutionCheckpointResponse::CONFIRMED_TERMINAL &&
        !execution_recovery::matches_execution(*auth, cp)) co_return;
    const auto* active = missions_.find(id);
    if (active && (active->robot_id != doc.robot_id || active->map_id != doc.map_id ||
                   !same_request(doc, *active))) co_return;
    const auto live = waiting_plans_.find(id);
    using Checkpoint = types::GetExecutionCheckpointResponse;

    if (cp.kind() == Checkpoint::ACTIVE) {
        // A durable ACTIVE may precede MQTT. Only the live view may resume execution.
        if (active && traffic_snapshot_ && live != waiting_plans_.end() &&
            (live->second.state() == types::MISSION_STATE_RUNNING ||
             live->second.state() == types::MISSION_STATE_PAUSED))
            co_await restore(id, job, epoch, std::move(commands));
        co_return;
    }
    if (cp.kind() == Checkpoint::UNCERTAIN) {
        // An observed failure is still visible to the operator. Its traffic protection stays open.
        if (live != waiting_plans_.end() && live->second.SerializeAsString() == cp.status().SerializeAsString() &&
            (cp.status().state() == types::MISSION_STATE_FAILED ||
             (auth->cancel && cp.status().state() == types::MISSION_STATE_CANCELED))) {
            if (auto* m = missions_.find(id)) apply(*m, cp.status(), cp.checkpoint_version());
        }
        co_return;
    }
    if (live != waiting_plans_.end() && newer_than(live->second, cp.status())) co_return;

    auto decision = execution_recovery::terminal(*auth, *graph, cp);
    if (!decision.terminal || !decision.held) co_return;
    const auto& base = *decision.held;
    auto held = traffic_routes_.find(doc.robot_id);
    if (held == traffic_routes_.end()) {
        traffic_routes_.emplace(doc.robot_id, base);
    } else if (held->second.key == base.key) {
        auto& old = held->second;
        if (old.version != base.version || old.map != base.map || old.nodes.size() > base.nodes.size() ||
            old.edges.size() > base.edges.size() ||
            !std::equal(old.nodes.begin(), old.nodes.end(), base.nodes.begin()) ||
            !std::equal(old.edges.begin(), old.edges.end(), base.edges.begin())) co_return;
        const auto reached = std::max(old.reached, base.reached);
        old = base;
        old.reached = reached;
    } else {
        // The final stream event can skip an intermediate step already covered by this same Add.
        const auto prior = std::ranges::find_if(auth->steps, [&](const auto& g) {
            return g.base.key == held->second.key;
        });
        if (prior == auth->steps.end()) co_return;
        const auto& old = held->second;
        const auto& previous = prior->base;
        if (old.map != previous.map || old.version != previous.version ||
            old.nodes.size() > previous.nodes.size() || old.edges.size() > previous.edges.size() ||
            !std::equal(old.nodes.begin(), old.nodes.end(), previous.nodes.begin()) ||
            !std::equal(old.edges.begin(), old.edges.end(), previous.edges.begin())) co_return;
        if (decision.preceding_steps_complete) held->second = base;
    }

    const std::string state = cp.status().state() == types::MISSION_STATE_COMPLETED ? "COMPLETED" : "CANCELED";
    if (auto* m = missions_.find(id)) {
        apply(*m, cp.status(), cp.checkpoint_version());
        job->delay = 200ms;
        co_return; // MissionStore writes the document and events in one transaction.
    }
    if (doc.state != state) co_return;
    Mission verified = doc;
    copy_status(verified, cp.status());
    if (mission_json(verified) != mission_json(doc)) co_return;
    if (!record.dispatch_final_version()) {
        // Upgrade an existing terminal document without adding duplicate history.
        db::SaveMissionRequest seal;
        *seal.mutable_mission() = record;
        seal.mutable_mission()->set_dispatch_final_version(cp.checkpoint_version());
        db::SaveMissionResponse ack;
        co_await services_.call<&Storage::PrepareAsyncSaveMission>(*services_.control, seal, ack);
        co_return;
    }
    if (record.dispatch_final_version() != cp.checkpoint_version()) co_return;
    auto pending = std::make_shared<PendingHandoff>();
    pending->epoch = epoch;
    pending->evidence = std::move(decision);
    pending->close.set_mission_id(id);
    pending->close.set_robot_id(doc.robot_id);
    pending->close.set_attempt(attempt);
    pending->close.set_dispatch_final_version(cp.checkpoint_version());
    for (const auto& c : commands) pending->close.add_command_ids(c.command_id());
    for (const auto& step : auth->steps) pending->owned_keys.push_back(step.base.key);
    job->handoff = std::move(pending);
    co_await handoff_checkpoint(id, job, epoch);
}

asio::awaitable<void> MissionRunner::handoff_checkpoint(std::string id, Job job, std::uint64_t epoch) {
    const auto handoff = job->handoff;
    const auto& base = *handoff->evidence.held;
    const auto safe = [&] {
        const auto* graph = maps_.find(base.map);
        const auto held = traffic_routes_.find(base.robot);
        return checkpoint_current(id, job, epoch) && graph && traffic_availability_ &&
               held != traffic_routes_.end() && held->second.map == base.map && held->second.version == base.version &&
               std::ranges::find(handoff->owned_keys, held->second.key) != handoff->owned_keys.end() &&
               handoff->evidence.stopped_on_node(*graph, traffic_availability_->live_status(base.robot));
    };
    if (!safe()) co_return;
    db::CloseMissionCommandsResponse ack;
    const auto status = co_await services_.call<&Storage::PrepareAsyncCloseMissionCommands>(
        *services_.control, handoff->close, ack);
    if (!status.ok() || !safe()) co_return;
    // No suspension between removing the corridor and enabling requests. The same
    // tracker supplies the occupied node to FixedRouteGate on its next check.
    traffic_routes_.erase(base.robot);
    extensions_.erase(id);
    waiting_plans_.erase(id);
    plans_.erase(id);
    uncertain_.erase(id);
    command_journal_.forget(id);
    journal_attempts_.erase(id);
    add_rpc_started_.erase(id);
    local_adds_.erase(id);
    release_next(base.robot, false, "");
    checkpoint_jobs_.erase(id);
}
