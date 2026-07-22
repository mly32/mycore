#include "dots/replication/replication.hpp"
#include "dots/simulation/world.hpp"
#include "dots/simulation/world_setup.hpp"

#include <catch2/catch_test_macros.hpp>
#include <variant>

TEST_CASE("Full snapshots map and sort authoritative entities", "[dots][replication]") {
    dots::simulation::World world;
    const auto food = world.spawn_food({3.0F, 4.0F});
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{0}, {1.0F, 2.0F});
    REQUIRE(food.has_value());
    REQUIRE(player.has_value());

    const auto result = dots::replication::build_full_snapshot(
        world, dots::protocol::SnapshotId{7}, dots::protocol::InputSequenceId{5}, 4);
    const auto* snapshot = std::get_if<dots::protocol::FullSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->snapshot_id == dots::protocol::SnapshotId{7});
    REQUIRE(snapshot->last_processed_input_id == dots::protocol::InputSequenceId{5});
    REQUIRE(snapshot->pending_input_count == 4);
    REQUIRE(snapshot->entities.size() == 2);
    CHECK(snapshot->entities[0].entity_id == dots::replication::to_protocol(*food));
    CHECK(snapshot->entities[0].kind == dots::protocol::EntityKind::Food);
    CHECK(snapshot->entities[1].entity_id == dots::replication::to_protocol(*player));
    CHECK(snapshot->entities[1].kind == dots::protocol::EntityKind::Player);
}

TEST_CASE("Replicated worlds replace newer state and reject stale snapshots",
          "[dots][replication]") {
    dots::replication::ReplicatedWorld world;
    const dots::protocol::FullSnapshot first{
        .snapshot_id = dots::protocol::SnapshotId{1},
        .server_tick = 2,
        .last_processed_input_id = dots::protocol::InputSequenceId{3},
        .pending_input_count = 2,
        .entities =
            {
                {
                    .entity_id = dots::protocol::EntityId{9},
                    .kind = dots::protocol::EntityKind::Player,
                    .position_x = 4.0F,
                    .mass = 16.0F,
                },
                {
                    .entity_id = dots::protocol::EntityId{2},
                    .kind = dots::protocol::EntityKind::Food,
                    .mass = 1.0F,
                },
            },
    };
    REQUIRE(world.apply(first) == dots::replication::SnapshotApplyResult::Applied);
    REQUIRE(world.entities().front().entity_id == dots::protocol::EntityId{2});
    REQUIRE(world.find(dots::protocol::EntityId{9}) != nullptr);
    REQUIRE(world.player_count() == 1);
    REQUIRE(world.food_count() == 1);
    REQUIRE(world.pending_input_count() == 2);

    auto stale = first;
    stale.entities.clear();
    REQUIRE(world.apply(stale) == dots::replication::SnapshotApplyResult::Stale);
    REQUIRE(world.entities().size() == 2);

    const dots::protocol::FullSnapshot replacement{
        .snapshot_id = dots::protocol::SnapshotId{2},
        .server_tick = 4,
        .entities = {{
            .entity_id = dots::protocol::EntityId{12},
            .kind = dots::protocol::EntityKind::Player,
            .mass = 25.0F,
        }},
    };
    REQUIRE(world.apply(replacement) == dots::replication::SnapshotApplyResult::Applied);
    REQUIRE(world.entities().size() == 1);
    REQUIRE(world.find(dots::protocol::EntityId{9}) == nullptr);
}

TEST_CASE("Replicated worlds reject invalid state atomically", "[dots][replication]") {
    dots::replication::ReplicatedWorld world;
    const dots::protocol::FullSnapshot invalid{
        .snapshot_id = dots::protocol::SnapshotId{1},
        .entities = {{
            .entity_id = dots::protocol::EntityId{2},
            .kind = dots::protocol::EntityKind::Food,
            .mass = 0.0F,
        }},
    };
    REQUIRE(world.apply(invalid) == dots::replication::SnapshotApplyResult::Invalid);
    REQUIRE_FALSE(world.snapshot_id().is_valid());
    REQUIRE(world.entities().empty());

    auto invalid_queue_depth = invalid;
    invalid_queue_depth.pending_input_count = dots::protocol::kMaximumPendingInputCount + 1;
    invalid_queue_depth.entities.front().mass = 1.0F;
    REQUIRE(world.apply(invalid_queue_depth) == dots::replication::SnapshotApplyResult::Invalid);
    REQUIRE_FALSE(world.snapshot_id().is_valid());
}

TEST_CASE("Default Dots food field is shared simulation setup", "[dots][replication]") {
    dots::simulation::World world;
    REQUIRE(dots::simulation::spawn_default_food_field(world));
    REQUIRE(world.food_count() == 272);
    REQUIRE(world.player_count() == 0);
}
