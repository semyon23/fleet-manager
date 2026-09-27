#pragma once
#include "common.hpp"
#include "types.hpp"

#include "connection.hpp"
#include "factsheet.hpp"
#include "instant_actions.hpp"
#include "order.hpp"
#include "responses.hpp"
#include "state.hpp"
#include "visualization.hpp"
#include "zone_set.hpp"
#include <set>

const std::set<std::string> kOrderRejectionErrors = {
    vda5050::error_type::kValidationFailure,
    vda5050::error_type::kUnsupportedParameter,
    vda5050::error_type::kInvalidOrderAction,
    vda5050::error_type::kOutdatedOrderUpdate,
    vda5050::error_type::kSameOrderUpdateId,
    vda5050::error_type::kOrderUpdateFollowingCancel,
    vda5050::error_type::kOtherOrderActive,
    vda5050::error_type::kStartNodeOutOfRange,
    vda5050::error_type::kNoRouteToTarget,
    vda5050::error_type::kMobileRobotNotAvailable,
    vda5050::error_type::kUnknownMapId,
};
inline vda5050::ErrorReference error_ref(const std::string& key, const std::string& value) {
    return {key, value};
}