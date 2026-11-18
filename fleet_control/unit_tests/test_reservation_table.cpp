#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

#include "traffic/ReservationTable.hpp"

namespace {

using traffic::Interval;
using traffic::ReservationTable;
using traffic::ResourceId;
using traffic::RobotId;
using traffic::Time;
using traffic::TimedReservation;
using traffic::kForever;

using Intervals = std::vector<Interval>;
using Plan = std::vector<TimedReservation>;

constexpr RobotId robot1 = 1;
constexpr RobotId robot2 = 2;
constexpr RobotId robot3 = 3;

// Три независимых узла.
// Геометрия здесь не нужна: проверяем время владения ресурсами.
traffic::ResourceMap test_resources() {
    Graph graph;
    graph.map_id = "test-time";
    graph.version = 1;

    for (const char* id : {"a", "b", "c"}) {
        GraphNode node;
        node.id = id;
        graph.nodes.push_back(node);
    }

    return traffic::ResourceMap{graph};
}

}  // namespace

TEST_CASE("time reservations: empty resource is free", "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    CHECK(table.safe_intervals(a, {0, 20}) ==
          Intervals{{0, 20}});

    CHECK(table.safe_intervals(a, {5, kForever}) ==
          Intervals{{5, kForever}});
}

TEST_CASE("time reservations: free intervals are clipped to window",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan first = {
        {a, {2, 5}},
        {a, {7, 9}}
    };

    REQUIRE_FALSE(table.try_replace(robot1, first));

    CHECK(table.safe_intervals(a, {0, 12}) ==
          Intervals{{0, 2}, {5, 7}, {9, 12}});

    CHECK(table.safe_intervals(a, {3, 8}) ==
          Intervals{{5, 7}});

    CHECK(table.safe_intervals(a, {3, 4}).empty());

    CHECK(table.safe_intervals(a, {9, 12}) ==
          Intervals{{9, 12}});
}

TEST_CASE("time reservations: touching intervals do not conflict",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan first = {{a, {2, 5}}};
    const Plan second = {{a, {5, 7}}};
    const Plan third = {{a, {0, 2}}};

    REQUIRE_FALSE(table.try_replace(robot1, first));
    CHECK_FALSE(table.try_replace(robot2, second));
    CHECK_FALSE(table.try_replace(robot3, third));

    CHECK(table.safe_intervals(a, {0, 10}) ==
          Intervals{{7, 10}});
}

TEST_CASE("time reservations: overlap reports owner and intersection",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan first = {{a, {2, 5}}};
    const Plan second = {{a, {4, 8}}};

    REQUIRE_FALSE(table.try_replace(robot1, first));

    auto conflict = table.try_replace(robot2, second);

    REQUIRE(conflict);
    CHECK(conflict->resource == a);
    CHECK(conflict->owner == robot1);
    CHECK(conflict->interval == Interval{4, 5});

    CHECK(table.safe_intervals(a, {0, 10}) ==
          Intervals{{0, 2}, {5, 10}});
}

TEST_CASE("time reservations: own plan may overlap during replacement",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan old_plan = {{a, {2, 5}}};
    const Plan new_plan = {{a, {4, 8}}};

    REQUIRE_FALSE(table.try_replace(robot1, old_plan));
    CHECK_FALSE(table.try_replace(robot1, new_plan));

    CHECK(table.safe_intervals(a, {0, 10}) ==
          Intervals{{0, 4}, {8, 10}});
}

TEST_CASE("time reservations: conflicting replacement keeps entire old plan",
          "[traffic-time]") {
    ReservationTable table(test_resources());

    const auto a = table.resources().node_resource(0);
    const auto b = table.resources().node_resource(1);
    const auto c = table.resources().node_resource(2);

    const Plan old_plan = {
        {a, {0, 4}},
        {c, {8, kForever}}
    };
    const Plan other = {
        {b, {5, 10}}
    };

    REQUIRE_FALSE(table.try_replace(robot1, old_plan));
    REQUIRE_FALSE(table.try_replace(robot2, other));

    const Plan replacement = {
        {a, {10, 12}},
        {b, {7, 9}}
    };

    auto conflict = table.try_replace(robot1, replacement);

    REQUIRE(conflict);
    CHECK(conflict->owner == robot2);

    // Старые резервы сохранены на обоих ресурсах.
    CHECK(table.safe_intervals(a, {0, 15}) ==
          Intervals{{4, 15}});

    CHECK(table.safe_intervals(c, {0, 15}) ==
          Intervals{{0, 8}});

    // Не появилось частичного нового резерва на a.
    CHECK(table.safe_intervals(a, {10, 12}) ==
          Intervals{{10, 12}});

    CHECK(table.safe_intervals(b, {0, 15}) ==
          Intervals{{0, 5}, {10, 15}});
}

TEST_CASE("time reservations: replacement removes resources no longer used",
          "[traffic-time]") {
    ReservationTable table(test_resources());

    const auto a = table.resources().node_resource(0);
    const auto b = table.resources().node_resource(1);

    const Plan old_plan = {{a, {0, kForever}}};
    const Plan new_plan = {{b, {5, kForever}}};

    REQUIRE_FALSE(table.try_replace(robot1, old_plan));
    CHECK_FALSE(table.try_replace(robot1, new_plan));

    CHECK(table.safe_intervals(a, {0, 10}) ==
          Intervals{{0, 10}});

    CHECK(table.safe_intervals(b, {0, 10}) ==
          Intervals{{0, 5}});
}

TEST_CASE("time reservations: destination remains occupied",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan arrived = {{a, {5, kForever}}};
    REQUIRE_FALSE(table.try_replace(robot1, arrived));

    CHECK(table.safe_intervals(a, {0, kForever}) ==
          Intervals{{0, 5}});

    const Plan another_robot = {{a, {1000, 1001}}};
    CHECK(table.try_replace(robot2, another_robot).has_value());

    // Согласован выезд: прежняя бессрочная занятость
    // заменяется занятостью до 12.
    const Plan departure = {{a, {5, 12}}};
    CHECK_FALSE(table.try_replace(robot1, departure));

    CHECK(table.safe_intervals(a, {0, kForever}) ==
          Intervals{{0, 5}, {12, kForever}});
}

TEST_CASE("time reservations: own intervals can be ignored for planning",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan first = {{a, {2, 5}}};
    const Plan second = {{a, {7, 9}}};

    REQUIRE_FALSE(table.try_replace(robot1, first));
    REQUIRE_FALSE(table.try_replace(robot2, second));

    CHECK(table.safe_intervals(a, {0, 12}, robot1) ==
          Intervals{{0, 7}, {9, 12}});

    CHECK(table.safe_intervals(a, {0, 12}, robot2) ==
          Intervals{{0, 2}, {5, 12}});

    // Запрос ничего в таблице не изменил.
    CHECK(table.safe_intervals(a, {0, 12}) ==
          Intervals{{0, 2}, {5, 7}, {9, 12}});
}

TEST_CASE("time reservations: unsorted overlapping own intervals are merged",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan plan = {
        {a, {7, 9}},
        {a, {2, 5}},
        {a, {4, 7}},
        {a, {2, 5}}
    };

    REQUIRE_FALSE(table.try_replace(robot1, plan));

    CHECK(table.safe_intervals(a, {0, 12}) ==
          Intervals{{0, 2}, {9, 12}});

    CHECK_FALSE(table.try_replace(robot1, plan));

    CHECK(table.safe_intervals(a, {0, 12}) ==
          Intervals{{0, 2}, {9, 12}});
}

TEST_CASE("time reservations: empty plan removes only its robot",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan first = {{a, {2, 5}}};
    const Plan second = {{a, {7, 9}}};

    REQUIRE_FALSE(table.try_replace(robot1, first));
    REQUIRE_FALSE(table.try_replace(robot2, second));

    const Plan empty;
    CHECK_FALSE(table.try_replace(robot1, empty));

    CHECK(table.safe_intervals(a, {0, 12}) ==
          Intervals{{0, 7}, {9, 12}});

    // Повтор удаления не влияет на другого робота.
    CHECK_FALSE(table.try_replace(robot1, empty));
    CHECK(table.safe_intervals(a, {0, 12}) ==
          Intervals{{0, 7}, {9, 12}});
}

TEST_CASE("time reservations: invalid plan leaves old plan unchanged",
          "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);

    const Plan original = {{a, {2, 5}}};
    REQUIRE_FALSE(table.try_replace(robot1, original));

    SECTION("negative time") {
        const Plan invalid = {{a, {-1, 5}}};
        CHECK_THROWS_AS(
            table.try_replace(robot1, invalid),
            std::invalid_argument);
    }

    SECTION("empty interval") {
        const Plan invalid = {{a, {5, 5}}};
        CHECK_THROWS_AS(
            table.try_replace(robot1, invalid),
            std::invalid_argument);
    }

    SECTION("reversed interval") {
        const Plan invalid = {{a, {7, 5}}};
        CHECK_THROWS_AS(
            table.try_replace(robot1, invalid),
            std::invalid_argument);
    }

    SECTION("invalid resource after a valid entry") {
        const auto invalid_resource =
            static_cast<ResourceId>(table.resources().size());

        const Plan invalid = {
            {a, {10, 12}},
            {invalid_resource, {12, 14}}
        };

        CHECK_THROWS_AS(
            table.try_replace(robot1, invalid),
            std::out_of_range);
    }

    CHECK(table.safe_intervals(a, {0, 15}) ==
          Intervals{{0, 2}, {5, 15}});
}

TEST_CASE("time reservations: invalid query is rejected", "[traffic-time]") {
    ReservationTable table(test_resources());
    const auto a = table.resources().node_resource(0);
    const auto invalid =
        static_cast<ResourceId>(table.resources().size());

    CHECK_THROWS_AS(
        table.safe_intervals(invalid, {0, 10}),
        std::out_of_range);

    CHECK_THROWS_AS(
        table.safe_intervals(a, {-1, 10}),
        std::invalid_argument);

    CHECK_THROWS_AS(
        table.safe_intervals(a, {5, 5}),
        std::invalid_argument);
}

TEST_CASE("time reservations: random plans match a simple time grid",
          "[traffic-time]") {
    ReservationTable table(test_resources());

    constexpr int resource_count = 3;
    constexpr int duration = 20;

    using Cell = std::optional<RobotId>;
    using Row = std::array<Cell, duration>;

    // Простой эталон: кто занимает каждый ресурс
    // в каждую целую миллисекунду [0, 20).
    std::array<Row, resource_count> expected{};

    std::mt19937 random(12345);

    for (int iteration = 0; iteration < 500; ++iteration) {
        CAPTURE(iteration);

        // RobotId 0 тоже допустим: свободное место обозначает optional.
        const RobotId robot = random() % 5;

        Plan plan;
        const int entries = random() % 6;

        for (int i = 0; i < entries; ++i) {
            const auto resource =
                static_cast<ResourceId>(random() % resource_count);

            const Time begin = random() % duration;
            const Time finite_end =
                begin + 1 + random() % (duration - begin);

            const Time end =
                random() % 5 == 0 ? kForever : finite_end;

            plan.push_back({resource, {begin, end}});
        }

        bool conflict_expected = false;

        for (const auto& item : plan) {
            const Time end =
                std::min(item.interval.end, Time{duration});

            for (Time time = item.interval.begin; time < end; ++time) {
                const Cell& held = expected[item.resource][time];

                if (held && *held != robot) {
                    conflict_expected = true;
                }
            }
        }

        auto conflict = table.try_replace(robot, plan);
        REQUIRE(conflict.has_value() == conflict_expected);

        if (!conflict_expected) {
            for (auto& row : expected) {
                for (Cell& cell : row) {
                    if (cell && *cell == robot) {
                        cell.reset();
                    }
                }
            }

            for (const auto& item : plan) {
                const Time end =
                    std::min(item.interval.end, Time{duration});

                for (Time time = item.interval.begin; time < end; ++time) {
                    expected[item.resource][time] = robot;
                }
            }
        }

        for (ResourceId resource = 0;
             resource < resource_count;
             ++resource) {

            const auto safe = table.safe_intervals(
                resource, {0, duration});

            for (const Interval& interval : safe) {
                CHECK(interval.begin >= 0);
                CHECK(interval.begin < interval.end);
                CHECK(interval.end <= duration);
            }

            for (int time = 0; time < duration; ++time) {
                int covering = 0;

                for (const Interval& interval : safe) {
                    if (interval.begin <= time && time < interval.end) {
                        ++covering;
                    }
                }

                CHECK(covering ==
                      (expected[resource][time] ? 0 : 1));
            }
        }
    }
}