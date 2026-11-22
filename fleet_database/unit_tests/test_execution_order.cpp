#include <catch2/catch_test_macros.hpp>
#include "ExecutionOrder.hpp"

TEST_CASE("Execution order uses absolute route indices", "[execution-order]") {
    fms::v1::MissionNode step;
    auto* move = step.mutable_move();
    for (auto id : {"A", "B", "C"}) move->add_nodes()->set_node_id(id);
    for (auto id : {"AB", "BC"}) move->add_edges()->set_edge_id(id);

    auto order = nlohmann::json::parse(R"({
        "nodes":[{"nodeId":"B","sequenceId":2,"released":true},
                 {"nodeId":"C","sequenceId":4,"released":true}],
        "edges":[{"edgeId":"BC","sequenceId":3,"released":true}]
    })");

    REQUIRE(execution_order::matches_route(order, step, 3, 1));
    CHECK_FALSE(execution_order::matches_route(order, step, 3, 0));
    CHECK_FALSE(execution_order::matches_route(order, step, 2, 1));

    SECTION("incorrect stitching node") {
        order["nodes"][0]["nodeId"] = "A";
        CHECK_FALSE(execution_order::matches_route(order, step, 3, 1));
    }

    SECTION("sequence restarted at zero") {
        order["nodes"][0]["sequenceId"] = 0;
        CHECK_FALSE(execution_order::matches_route(order, step, 3, 1));
    }

    SECTION("edge grants travel into horizon") {
        order["nodes"][1]["released"] = false;
        CHECK_FALSE(execution_order::matches_route(order, step, 2, 1));

        order["edges"][0]["released"] = false;
        CHECK(execution_order::matches_route(order, step, 2, 1));
    }
}