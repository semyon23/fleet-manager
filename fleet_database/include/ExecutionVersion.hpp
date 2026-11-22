#ifndef FLEETMANAGERSYSTEM_EXECUTIONVERSION_HPP
#define FLEETMANAGERSYSTEM_EXECUTIONVERSION_HPP

#pragma once
#include <cstdint>

namespace execution_version {

    enum class Decision { advance, replay, stale, closed };

    inline Decision decide(std::uint64_t stored, bool terminal, std::uint64_t incoming) {
        if (incoming < stored) return Decision::stale;
        if (incoming == stored) return Decision::replay;
        return terminal ? Decision::closed : Decision::advance;
    }

}  // namespace execution_version


#endif //FLEETMANAGERSYSTEM_EXECUTIONVERSION_HPP
