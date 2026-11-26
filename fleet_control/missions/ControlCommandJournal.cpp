#include "ControlCommandJournal.hpp"

#include <stdexcept>
#include <spdlog/spdlog.h>

namespace db = fms::database::v1;

ControlCommandJournal::Handle ControlCommandJournal::stage(Record record) {
    auto& commands = entries_[record.mission_id()];
    auto found = commands.find(record.command_id());

    if (found != commands.end()) {
        const auto& saved = found->second->record;
        if (saved.robot_id() != record.robot_id() ||
            saved.map_id() != record.map_id() ||
            saved.map_version() != record.map_version() ||
            saved.kind() != record.kind() ||
            saved.attempt() != record.attempt() ||
            saved.move_node_id() != record.move_node_id() ||
            saved.revision() != record.revision() ||
            saved.request_proto() != record.request_proto()) {
            throw std::logic_error("staged command identity changed");
        }
        // Повтор сохраняет снимок, сделанный перед первой отправкой.
        return found->second;
    }

    auto entry = std::make_shared<Entry>(std::move(record));
    commands.emplace(entry->record.command_id(), entry);
    return entry;
}

asio::awaitable<grpc::Status> ControlCommandJournal::persist(Handle entry) {
    if (entry->resolved) {
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               "command journal entry is already resolved"};
    }
    if (entry->stored) co_return grpc::Status::OK;

    db::PutControlCommandRequest request;
    *request.mutable_command() = entry->record;
    db::PutControlCommandResponse response;
    const auto status = co_await services_.call<
        &db::ControlStorage::Stub::PrepareAsyncPutControlCommand>(
            *services_.control, request, response);

    if (!status.ok()) {
        // Пока ждали, другой вызов мог получить подтверждение той же записи.
        if (entry->stored && !entry->resolved) co_return grpc::Status::OK;
        if (entry->last_error != status.error_message()) {
            entry->last_error = status.error_message();
            spdlog::warn("[journal] command {} not acknowledged: {}",
                         entry->record.command_id(), entry->last_error);
        }
        co_return status;
    }

    entry->stored = true;
    entry->resolved = entry->resolved || response.resolved();
    if (entry->resolved) {
        spdlog::error("[journal] refusing to execute resolved command {}",
                      entry->record.command_id());
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               "command journal entry is already resolved"};
    }
    co_return grpc::Status::OK;
}

void ControlCommandJournal::forget(const std::string& mission_id) {
    // Удаляем только из памяти. Записи БД остаются для разбора при старте.
    entries_.erase(mission_id);
}

bool ControlCommandJournal::all_stored(const std::string& mission_id) const {
    const auto found = entries_.find(mission_id);
    if (found == entries_.end()) return true;
    for (const auto& [id, entry] : found->second)
        if (!entry->stored || entry->resolved) return false;
    return true;
}

bool ControlCommandJournal::merge_stored(const std::string& mission_id,
                                        std::vector<Record>& records, Missing missing) const {
    std::map<std::string, std::size_t> known;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& row = records[i];
        if (row.mission_id() != mission_id || row.command_id().empty() ||
            !known.emplace(row.command_id(), i).second) return false;
    }
    const auto found = entries_.find(mission_id);
    if (found == entries_.end()) return true;
    for (const auto& [key, entry] : found->second) {
        if (!entry->stored || entry->resolved) return false;
        const auto it = known.find(key);
        if (it == known.end()) {
            if (missing == Missing::Reject) return false;
            records.push_back(entry->record);
        } else if (records[it->second].SerializeAsString() != entry->record.SerializeAsString()) {
            return false;
        }
    }
    return true;
}
