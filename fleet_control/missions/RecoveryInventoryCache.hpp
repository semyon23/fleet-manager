#pragma once

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <google/protobuf/util/message_differencer.h>
#include "fleet_dispatch.pb.h"

namespace recovery_assessment {

class InventoryCache {
public:
    using Clock = std::chrono::steady_clock;
    using Reply = fms::dispatch::v1::GetRecoveryInventoryResponse;

    bool accept(Reply reply, Clock::time_point expires) {
        invalidate();
        if (reply.instance_id().empty() || !reply.revision() ||
            retired_.contains(reply.instance_id())) return false;

        if (reply.instance_id() != latest_.instance_id()) {
            if (!latest_.instance_id().empty()) retired_.insert(latest_.instance_id());
            inconsistent_ = false;
        } else {
            if (reply.revision() < latest_.revision()) return false;
            if (reply.revision() == latest_.revision()) {
                if (!google::protobuf::util::MessageDifferencer::Equals(latest_, reply))
                    inconsistent_ = true;
                if (inconsistent_) return false;
            } else {
                inconsistent_ = false;
            }
        }
        latest_.Swap(&reply);
        expires_ = expires;
        return true;
    }

    void invalidate() { expires_.reset(); }
    const Reply* current(Clock::time_point now) const {
        return expires_ && now < *expires_ ? &latest_ : nullptr;
    }
    const Reply& latest() const { return latest_; }

private:
    Reply latest_;
    std::set<std::string> retired_;
    std::optional<Clock::time_point> expires_;
    bool inconsistent_ = false;
};
} // namespace recovery_assessment
