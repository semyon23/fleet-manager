#include <catch2/catch_test_macros.hpp>
#include "ExecutionVersion.hpp"

TEST_CASE("Dispatch checkpoint cannot regress or reopen", "[execution-version]") {
    using namespace execution_version;

    CHECK(decide(0, false, 1) == Decision::advance);
    CHECK(decide(3, false, 5) == Decision::advance);
    CHECK(decide(3, false, 2) == Decision::stale);
    CHECK(decide(3, false, 3) == Decision::replay);

    CHECK(decide(3, true, 3) == Decision::replay);
    CHECK(decide(3, true, 2) == Decision::stale);
    CHECK(decide(3, true, 4) == Decision::closed);
}