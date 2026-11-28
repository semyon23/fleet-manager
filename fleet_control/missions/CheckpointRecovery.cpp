#include "missions/MissionRunner.hpp"
#include "fleet/Availability.hpp"
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

recovery_assessment::LocalEvidence local_evidence(const execution_recovery::Authority& auth,
                                                  bool recovering) {
    recovery_assessment::LocalEvidence result;
    result.robot = auth.add.mission().robot_id();
    result.attempt = auth.add.attempt();
    result.recovering = recovering;
    result.steps.reserve(auth.steps.size());
    for (const auto& step : auth.steps) result.steps.push_back(step.base);
    return result;
}

// Keep the previous proof available while its exact epoch is rechecked. A
// failed refresh revokes it, including exceptions and RPC errors.
struct ProofRefresh {
    std::optional<recovery_assessment::LocalEvidence>& proof;
    bool checked = false;
    ~ProofRefresh() { if (!checked) proof.reset(); }
};
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
        if (traffic_snapshot_ && (checkpoint_jobs_.size() == 1 || current_inventory()))
            invalidate_inventory();
        checkpoint_timer_.cancel_one();
    }
}

bool MissionRunner::checkpoint_current(const std::string& id, const Job& job, std::uint64_t epoch) const {
    const auto it = checkpoint_jobs_.find(id);
    return !stopped_ && it != checkpoint_jobs_.end() && it->second == job && job->epoch == epoch;
}

std::optional<recovery_assessment::Local> MissionRunner::assessment_evidence(const std::string& pending_add) const {
    if (!journal_loaded_) return std::nullopt;
    auto unaccounted = sending_.size();
    recovery_assessment::Local owners;
    for (const auto& [id, job] : checkpoint_jobs_) {
        if (!job->assessment || job->proof_epoch != job->epoch ||
            job->proof_source_epoch != assessment_source_epoch_ ||
            job->assessment->attempt != journal_attempt(id)) return std::nullopt;
        if (!command_journal_.all_stored(id)) return std::nullopt;
        owners.emplace(id, *job->assessment);
    }
    for (const auto* mission : missions_.active()) {
        const auto& id = mission->id;
        if (sending_.contains(id)) {
            if (!local_adds_.contains(id) || add_rpc_started_.contains(id) || owners.contains(id) ||
                mission->state != "ASSIGNED" || !command_journal_.all_stored(id)) return std::nullopt;
            --unaccounted;
            if (id == pending_add) continue; // assess() checks its exact origin and dispatch ownership.
            const auto* view = current_inventory();
            const auto* graph = maps_.find(mission->map_id);
            const auto held = traffic_routes_.find(mission->robot_id);
            if (!view || !graph || held == traffic_routes_.end() ||
                std::ranges::find(view->blocked_robots(), mission->robot_id) != view->blocked_robots().end() ||
                !traffic_availability_) return std::nullopt;
            const auto* robot = traffic_availability_->live_status(mission->robot_id, view->instance_id());
            if (!robot || !robot->mission_id().empty() || !robot->observed_order().idle() ||
                !stopped_on_node(*robot, *graph, robot->last_node_id())) return std::nullopt;
            RecoveryCommands records;
            if (!command_journal_.merge_stored(id, records, ControlCommandJournal::Missing::Append))
                return std::nullopt;
            const auto auth = execution_recovery::read_authority(*mission, *graph, records, journal_attempt(id));
            if (!auth || auth->cancel || auth->steps.empty() ||
                std::ranges::any_of(auth->steps, [](const auto& grant) { return grant.revision != 0; }))
                return std::nullopt;
            const auto& origin = auth->steps.front().base;
            if (origin.nodes.size() != 1 || graph->find_node(robot->last_node_id()) != origin.nodes.front() ||
                held->second.key != origin.key || held->second.nodes != origin.nodes ||
                !held->second.edges.empty() || held->second.reached != 0) return std::nullopt;
            owners.emplace(id, recovery_assessment::LocalEvidence{mission->robot_id, journal_attempt(id), {origin}, true});
            continue;
        }
        if (const auto it = owners.find(id); it != owners.end()) {
            if (mission->robot_id != it->second.robot || it->second.steps.empty() ||
                mission->map_id != it->second.steps.front().map) return std::nullopt;
            continue;
        }
        const auto next = next_.find(mission->robot_id);
        if (mission->state == "QUEUED" || (next != next_.end() && next->second.mission_id == id)) continue;
        const auto plan = plans_.find(id);
        const auto* graph = maps_.find(mission->map_id);
        if (plan == plans_.end() || !graph || graph->version != plan->second.traffic_version)
            return std::nullopt;
        const auto* source = current_inventory();
        if (!source || plan->second.status_instance != source->instance_id()) return std::nullopt;
        auto& cache = plan->second.assessment;
        if (!cache) {
            auto records = plan->second.restored_commands;
            if (!command_journal_.merge_stored(id, records, ControlCommandJournal::Missing::Append))
                return std::nullopt;
            const auto auth = execution_recovery::read_authority(*mission, *graph, records, journal_attempt(id));
            if (!auth) return std::nullopt;
            cache.emplace(Plan::Assessment{*mission, local_evidence(*auth, auth->cancel)});
        }
        const auto& saved = cache->request;
        if (!command_journal_.all_stored(id) || saved.id != id || saved.created_at_ms != mission->created_at_ms ||
            !same_request(saved, *mission) || cache->evidence.robot != mission->robot_id ||
            cache->evidence.attempt != journal_attempt(id)) return std::nullopt;
        auto& copy = owners.emplace(id, cache->evidence).first->second;
        copy.recovering = copy.recovering || uncertain_.contains(id) ||
            (mission->state != "RUNNING" && mission->state != "PAUSED");
    }
    if (unaccounted) return std::nullopt;
    return owners;
}

// Bind the observation to the checkpoint epoch. An extension consumes it only
// after reassessing the complete fleet and the traffic reservation.
asio::awaitable<void> MissionRunner::observe_checkpoint(const std::string& id, const Job& job,
    std::uint64_t epoch, const execution_recovery::Checkpoint& cp, const RecoveryCommands& journal) {
    const auto* view = current_inventory();
    if (!view || view->state() != Inventory::READY || !view->has_live_restrictions() ||
        !traffic_availability_ || !job->assessment || job->assessment->steps.empty()) co_return;
    const auto& robot_id = cp.robot_id();
    const auto& restricted = view->live_restrictions().robots();
    if (std::ranges::find(restricted, robot_id) != restricted.end() ||
        std::ranges::find(view->robots(), robot_id) == view->robots().end() ||
        std::ranges::find(view->blocked_robots(), robot_id) == view->blocked_robots().end()) co_return;
    bool owned = false;
    for (const auto& row : view->missions()) {
        if (row.mission_id() != id && row.robot_id() != robot_id) continue;
        if (owned || row.mission_id() != id || row.robot_id() != robot_id || row.uncertain() ||
            !row.has_add_attempt() || row.add_attempt() != cp.add_attempt()) co_return;
        owned = true;
    }
    if (!owned || !traffic_availability_->live_status(robot_id, view->instance_id())) co_return;

    dp::GetMissionRequest req;
    req.set_mission_id(id);
    req.set_expected_robot_id(robot_id);
    req.set_expected_add_attempt(cp.add_attempt());
    req.set_expected_instance_id(view->instance_id());
    const auto revision = view->revision();
    const auto source = assessment_source_epoch_;
    const auto expires = std::chrono::steady_clock::now() + 3s;
    dp::GetMissionResponse reply;
    const auto result = co_await services_.call<&Dispatch::PrepareAsyncGetMission>(*services_.dispatch, req, reply);
    job->observed_until = {};
    view = current_inventory();
    if (!result.ok() || !checkpoint_current(id, job, epoch) || traffic_snapshot_ ||
        source != assessment_source_epoch_ || !view || view->instance_id() != req.expected_instance_id() ||
        view->revision() != revision || std::chrono::steady_clock::now() >= expires ||
        !job->assessment || job->assessment->steps.empty() ||
        job->proof_epoch != epoch || job->proof_source_epoch != source ||
        !command_journal_.all_stored(id) || journal_attempt(id) != cp.add_attempt()) co_return;
    auto* m = missions_.find(id);
    if (!m || m->robot_id != robot_id || (m->state != "ASSIGNED" && m->state != "RUNNING")) co_return;
    const auto* graph = maps_.find(m->map_id);
    const auto& proof = job->assessment->steps.front();
    if (!graph || proof.map != graph->map_id || proof.version != graph->version) co_return;
    const auto* robot = traffic_availability_->live_status(robot_id, req.expected_instance_id());
    if (!robot) co_return;
    const auto* prior = job->observed ? &*job->observed : nullptr;
    auto move = execution_recovery::observe_move(cp, req, reply, *robot, prior);
    if (!move && recovery_move(*m) && reply.has_unpublished_revision()) {
        const auto pending = extensions_.find(id);
        if (pending != extensions_.end() && pending->second.requires_scope() &&
            pending->second.state() == ExtensionCommand::State::DispatchPending) {
            move = execution_recovery::observe_unpublished(cp, req, reply, *robot, prior,
                                                           pending->second.request(), journal);
        }
    }
    if (!move || move->step < m->current_step.value_or(0)) co_return;
    const auto old = plans_.find(id);
    if (old != plans_.end() && move->step < static_cast<int>(old->second.progress.size())) {
        const auto& prior = old->second.progress[move->step];
        if (move->progress.last_node_index() < prior.last_node_index() ||
            move->progress.revision() < prior.revision() ||
            move->progress.released_nodes() < prior.released_nodes()) co_return;
    }
    if (old != plans_.end() && recovery_move(*m) && move->step == 0) {
        // The Add response can precede the first running status. Accept only
        // the status whose route, step, attempt and robot we just verified.
        uncertain_.erase(id);
        update_plan(old->second, reply.status());
        observe_extension(reply.status());
        if (m->state == "ASSIGNED" || !m->current_step || m->step_states.empty() ||
            m->step_states.front().state != "RUNNING") {
            copy_status(*m, reply.status());
            missions_.changed(*m, "running move confirmed by robot during recovery");
        }
    }
    job->observed = std::move(move);
    job->observed_revision = revision;
    job->observed_source = source;
    job->observed_epoch = epoch;
    job->observed_until = expires;
    job->delay = 500ms;
}

const execution_recovery::ObservedMove* MissionRunner::scoped_progress(const Mission& mission) const {
    const auto it = checkpoint_jobs_.find(mission.id);
    const auto* view = current_inventory();
    if (!view || !traffic_availability_ || it == checkpoint_jobs_.end() ||
        (mission.state != "ASSIGNED" && mission.state != "RUNNING")) return nullptr;
    const auto& j = *it->second;
    if (!j.observed || !j.assessment || j.assessment->steps.empty() ||
        j.observed_source != assessment_source_epoch_ || j.observed_epoch != j.epoch ||
        j.observed_revision != view->revision() || j.observed->add_attempt != journal_attempt(mission.id) ||
        !command_journal_.all_stored(mission.id) || std::chrono::steady_clock::now() >= j.observed_until)
        return nullptr;
    const auto* robot = traffic_availability_->live_status(mission.robot_id, view->instance_id());
    const auto* graph = maps_.find(mission.map_id);
    const auto& base = j.assessment->steps.front();
    if (!robot || !robot->localized() || !robot->has_pose() || robot->pose().map_id() != mission.map_id ||
        !graph || graph->map_id != base.map || graph->version != base.version ||
        !recovery_assessment::matches_move(*robot, mission.id, j.observed->step, j.observed->progress))
        return nullptr;
    return &*j.observed;
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
    const auto source_epoch = assessment_source_epoch_;
    const bool refresh_only = job->handoff && job->handoff->epoch == epoch;
    if (refresh_only) {
        co_await handoff_checkpoint(id, job, epoch);
        if (!current() || (job->assessment && job->proof_epoch == epoch &&
                           job->proof_source_epoch == source_epoch)) co_return;
    }
    ProofRefresh refresh{job->assessment};

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
        if (refresh_only) co_return;
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
        // Локальная отмена окончательно закрыта ответами БД.
        execution_recovery::finish_checkpoint_job(checkpoint_jobs_, id, on_checkpoint_jobs_drained);
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
    if (!refresh_only && !traffic_routes_.contains(doc.robot_id)) {
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
    if (refresh_only && (cp.kind() != types::GetExecutionCheckpointResponse::CONFIRMED_TERMINAL ||
        cp.checkpoint_version() != job->handoff->close.dispatch_final_version())) co_return;
    if (cp.kind() != types::GetExecutionCheckpointResponse::ACTIVE ||
        job->version != cp.checkpoint_version()) job->observed_until = {};
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
    const auto remember = [&] {
        job->assessment.reset();
        if (source_epoch != assessment_source_epoch_ || journal_attempt(id) != attempt) return;
        if (std::ranges::any_of(rows.commands(), [&](const auto& row) {
            return row.command().attempt() == attempt && row.resolved();
        })) return;
        // A new command may be staged or persisted while the list RPC is in
        // flight. It must occur in this same durable command set.
        if (!command_journal_.merge_stored(id, commands, ControlCommandJournal::Missing::Reject)) return;
        job->assessment = local_evidence(*auth, true);
        job->proof_epoch = epoch;
        job->proof_source_epoch = source_epoch;
        refresh.checked = true;
    };

    if (cp.kind() == Checkpoint::ACTIVE) {
        remember();
        // A durable ACTIVE may precede MQTT. Only the live view may resume execution.
        if (auth->cancel) job->observed_until = {};
        if (active && !traffic_snapshot_ && refresh.checked && !auth->cancel)
            co_await observe_checkpoint(id, job, epoch, cp, commands);
        else if (active && traffic_snapshot_ && live != waiting_plans_.end() &&
            (live->second.state() == types::MISSION_STATE_RUNNING ||
             live->second.state() == types::MISSION_STATE_PAUSED))
            co_await restore(id, job, epoch, std::move(commands));
        co_return;
    }
    job->observed_until = {};
    if (cp.kind() == Checkpoint::UNCERTAIN) {
        remember();
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
    if (refresh_only) {
        remember();
        co_return;
    }
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

    remember();

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
    // БД подтвердила закрытие. Теперь планировщик может проверить новую миссию.
    execution_recovery::finish_checkpoint_job(checkpoint_jobs_, id, on_checkpoint_jobs_drained);
}
