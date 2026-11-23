#include <catch2/catch_test_macros.hpp>
#include "ExecutionCheckpoint.hpp"

TEST_CASE("Read the durable dispatch checkpoint", "[execution-checkpoint]") {
    using namespace fms::v1;
    using Result = GetExecutionCheckpointResponse;
    using execution_checkpoint::classify;
    fms::database::v1::MissionExecution execution;
    auto* status = execution.mutable_status();

    SECTION("active checkpoint needs a step and consistent flags") {
        status->set_current_node_id("move");
        CHECK(classify(execution, false, false) == Result::UNCERTAIN);
        status->set_state(MISSION_STATE_RUNNING);
        CHECK(classify(execution, false, false) == Result::ACTIVE);
        CHECK(classify(execution, false, true) == Result::UNCERTAIN);
        CHECK(classify(execution, true, false) == Result::UNCERTAIN);
        status->set_state(MISSION_STATE_PAUSED);
        CHECK(classify(execution, false, false) == Result::ACTIVE);
        status->set_state(MISSION_STATE_CANCELING);
        CHECK(classify(execution, false, false) == Result::UNCERTAIN);
        execution.set_cancel_action_id("cancel");
        CHECK(classify(execution, false, false) == Result::ACTIVE);
        status->clear_current_node_id();
        CHECK(classify(execution, false, false) == Result::UNCERTAIN);
    }

    SECTION("completed mission can contain a skipped final step") {
        status->set_state(MISSION_STATE_COMPLETED);
        status->add_steps()->set_state(STEP_STATE_SKIPPED);
        CHECK(classify(execution, true, false) == Result::CONFIRMED_TERMINAL);
        CHECK(classify(execution, true, true) == Result::UNCERTAIN);
        CHECK(classify(execution, false, false) == Result::UNCERTAIN);
        status->set_current_node_id("move");
        CHECK(classify(execution, true, false) == Result::UNCERTAIN);
        status->clear_current_node_id();
        execution.mutable_order()->set_order_id("order");
        CHECK(classify(execution, true, false) == Result::UNCERTAIN);
    }

    SECTION("only a stopped robot with an identified cancel can confirm cancellation") {
        status->set_state(MISSION_STATE_CANCELED);
        execution.set_stopped_after_cancel(true);
        CHECK(classify(execution, true, false) == Result::UNCERTAIN);
        execution.set_cancel_action_id("cancel");
        CHECK(classify(execution, true, false) == Result::CONFIRMED_TERMINAL);
        execution.set_stopped_after_cancel(false);
        CHECK(classify(execution, true, false) == Result::UNCERTAIN);
        status->set_state(MISSION_STATE_FAILED);
        CHECK(classify(execution, true, false) == Result::UNCERTAIN);
    }
}