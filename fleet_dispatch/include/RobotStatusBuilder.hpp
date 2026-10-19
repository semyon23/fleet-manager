
#ifndef ROBOTSTATUSBUILDER_HPP
#define ROBOTSTATUSBUILDER_HPP

#include "fms_types.pb.h"
#include "vda5050.hpp"


void fill_status(const vda5050::State& state, fms::v1::RobotStatus& status);

fms::v1::ConnectionState to_proto(vda5050::ConnectionState state);




#endif //ROBOTSTATUSBUILDER_HPP
