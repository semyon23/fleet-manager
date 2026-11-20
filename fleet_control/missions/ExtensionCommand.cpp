#include "ExtensionCommand.hpp"


ExtensionCommand::ExtensionCommand(fms::dispatch::v1::ExtendMissionRequest request, const fms::v1::MoveProgress &previous)
    : request_(std::move(request)) {

    if (request_.mission_id().empty() ||
        request_.move_node_id().empty() ||
        request_.has_replace_tail() ||
        !request_.has_released_nodes() ||
        previous.released_nodes() == 0 ||
        previous.released_nodes() > previous.total_nodes() ||
        previous.revision() != previous.confirmed_revision() ||
        !previous.update_rejected().empty() ||
        previous.revision() ==
            std::numeric_limits<std::uint32_t>::max() ||
        request_.revision() != previous.revision() + 1 ||
        request_.released_nodes() <= previous.released_nodes() ||
        request_.released_nodes() > previous.total_nodes()) {
        throw std::invalid_argument(
            "invalid extension command or unconfirmed previous base");
    }

    total_nodes_ = previous.total_nodes();
}

void ExtensionCommand::block(std::string reason) {
    if (state_ == State::Confirmed || state_ == State::Blocked) {
        return;
    }

    reason_ = std::move(reason);
    state_ = State::Blocked;
}

void ExtensionCommand::observe(const fms::v1::MoveProgress &progress) {
    if (state_ == State::Confirmed || state_ == State::Blocked) {
        return;
    }

    if (progress.revision() < request_.revision()) {
        return;
    }

    if (progress.revision() != request_.revision() ||
        progress.released_nodes() != request_.released_nodes() ||
        progress.total_nodes() != total_nodes_ ||
        progress.confirmed_revision() > progress.revision()) {
        block("extension progress differs from saved command");
        return;
        }

    if (!progress.update_rejected().empty()) {
        block(progress.update_rejected());
        return;
    }

    state_ = progress.confirmed_revision() == request_.revision()
        ? State::Confirmed
        : State::RobotPending;
}

const std::string &ExtensionCommand::reason() const {
    return reason_;
}

const fms::dispatch::v1::ExtendMissionRequest& ExtensionCommand::request() const {
    return request_;
}

ExtensionCommand::State ExtensionCommand::state() const {
    return state_;
}
