#include "missions/RecoveryInventoryCache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>

namespace {
using Cache = recovery_assessment::InventoryCache;
using Reply = Cache::Reply;
using namespace std::chrono_literals;

Reply inventory(const char* instance, std::uint64_t revision, Reply::State state = Reply::READY) {
    Reply reply;
    reply.set_instance_id(instance);
    reply.set_revision(revision);
    reply.set_state(state);
    return reply;
}
} // namespace

TEST_CASE("Inventory freshness and revision history survive separate events", "[recovery-assessment]") {
    Cache cache;
    const Cache::Clock::time_point now{};
    const auto expires = now + 3s;
    REQUIRE_FALSE(cache.current(now));
    REQUIRE(cache.accept(inventory("first", 5), expires));
    REQUIRE(cache.current(expires - 1ms));
    CHECK_FALSE(cache.current(expires));
    CHECK_FALSE(cache.current(expires + 1ms));
    // A successful identical refresh can make the same version current again.
    REQUIRE(cache.accept(inventory("first", 5), expires + 3s));
    REQUIRE(cache.current(expires));
    cache.invalidate();
    REQUIRE_FALSE(cache.current(expires));
    // An RPC outage clears freshness and preserves the highest observed revision.
    CHECK_FALSE(cache.accept(inventory("first", 4), expires + 4s));
    REQUIRE_FALSE(cache.current(expires));
    REQUIRE(cache.accept(inventory("first", 6), expires + 5s));
    REQUIRE(cache.current(expires));
    CHECK(cache.latest().revision() == 6);
}

TEST_CASE("One inventory revision identifies exactly one view", "[recovery-assessment]") {
    Cache cache;
    const Cache::Clock::time_point now{};
    const auto original = inventory("first", 7);
    REQUIRE(cache.accept(original, now + 3s));
    auto conflicting = original;
    conflicting.add_blocked_robots("A");
    REQUIRE_FALSE(cache.accept(conflicting, now + 3s));
    REQUIRE_FALSE(cache.current(now));
    // Repeating either side of the contradiction cannot restore this revision.
    CHECK_FALSE(cache.accept(original, now + 4s));
    CHECK_FALSE(cache.accept(conflicting, now + 4s));
    CHECK_FALSE(cache.current(now));
    REQUIRE(cache.accept(inventory("first", 8), now + 5s));
    REQUIRE(cache.current(now));
    CHECK(cache.current(now)->revision() == 8);
}

TEST_CASE("Dispatch generations have independent revisions and retired generations stay retired",
          "[recovery-assessment]") {
    Cache cache;
    const Cache::Clock::time_point now{};
    REQUIRE(cache.accept(inventory("first", 40), now + 3s));
    REQUIRE(cache.accept(inventory("second", 1, Reply::LOADING), now + 3s));
    REQUIRE(cache.current(now));
    CHECK(cache.current(now)->state() == Reply::LOADING);
    REQUIRE(cache.accept(inventory("second", 2), now + 4s));
    CHECK_FALSE(cache.accept(inventory("first", 41), now + 5s));
    // The old instance cannot become current, even with a higher revision.
    CHECK_FALSE(cache.current(now));
    CHECK(cache.latest().instance_id() == "second");
    REQUIRE(cache.accept(inventory("second", 3), now + 5s));
    REQUIRE(cache.current(now));
    CHECK(cache.current(now)->instance_id() == "second");
    CHECK(cache.current(now)->revision() == 3);
}
