#ifndef FLEETMANAGERSYSTEM_CONTROLCOMMANDJOURNAL_HPP
#define FLEETMANAGERSYSTEM_CONTROLCOMMANDJOURNAL_HPP

#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "services/Services.hpp"

class ControlCommandJournal {
public:
    using Record = fms::database::v1::ControlCommandRecord;

    struct Entry {
        explicit Entry(Record value) : record(std::move(value)) {}

        const Record record;
        bool stored = false;
        bool resolved = false;
        std::string last_error;
    };

    using Handle = std::shared_ptr<Entry>;

    explicit ControlCommandJournal(Services& services) : services_(services) {}

    Handle stage(Record record);
    asio::awaitable<grpc::Status> persist(Handle entry);
    void forget(const std::string& mission_id);
    // Merge local commands with a previously read journal. A staged command
    // without an acknowledgement makes the possible issued base ambiguous.
    enum class Missing { Append, Reject };
    bool all_stored(const std::string& mission_id) const;
    bool merge_stored(const std::string& mission_id, std::vector<Record>& records, Missing missing) const;

private:
    Services& services_;
    std::map<std::string, std::map<std::string, Handle>> entries_;
};


#endif //FLEETMANAGERSYSTEM_CONTROLCOMMANDJOURNAL_HPP
