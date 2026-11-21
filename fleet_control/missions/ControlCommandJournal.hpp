#ifndef FLEETMANAGERSYSTEM_CONTROLCOMMANDJOURNAL_HPP
#define FLEETMANAGERSYSTEM_CONTROLCOMMANDJOURNAL_HPP

#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

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

private:
    Services& services_;
    std::map<std::string, std::map<std::string, Handle>> entries_;
};


#endif //FLEETMANAGERSYSTEM_CONTROLCOMMANDJOURNAL_HPP
