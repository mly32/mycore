#include "dots/replication/replication.hpp"
#include "dots/simulation/world.hpp"
#include "dots/simulation/world_setup.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <variant>

namespace {

[[nodiscard]] dots::protocol::RecipientSessionState
playing_session(dots::protocol::EntityId primary_entity_id) {
    return {
        .mode = dots::protocol::SessionMode::Playing,
        .owned_entity_ids = {primary_entity_id},
        .primary_entity_id = primary_entity_id,
    };
}

} // namespace

TEST_CASE("Full snapshots map and sort authoritative entities", "[dots][replication]") {
    dots::simulation::World world;
    const auto food = world.spawn_food({3.0F, 4.0F});
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{0}, {1.0F, 2.0F});
    REQUIRE(food.has_value());
    REQUIRE(player.has_value());

    const auto result = dots::replication::build_full_snapshot(
        world,
        dots::protocol::SnapshotId{7},
        dots::protocol::InputSequenceId{5},
        4,
        playing_session(dots::replication::to_protocol(*player)));
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
    CHECK(snapshot->entities[1].owner_id == dots::protocol::PlayerOwnerId{0});
    CHECK(snapshot->owners.size() == 1);
    CHECK(snapshot->owners[0].owner_id == dots::protocol::PlayerOwnerId{0});
    CHECK(snapshot->checkpoint_schema_id == dots::protocol::kCheckpointSchemaId);
    CHECK(snapshot->checkpoint_digest != 0);
    CHECK(snapshot->next_entity_id == dots::protocol::EntityId{2});
}

TEST_CASE("Version 4 snapshots hydrate exact rollback checkpoints and verify their digest",
          "[dots][replication][rollback]") {
    dots::simulation::World world;
    const auto food = world.spawn_food({30.0F, 30.0F});
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{3}, {1.0F, 2.0F});
    REQUIRE(food.has_value());
    REQUIRE(player.has_value());
    const auto tick = world.advance({dots::simulation::TickCommand{
        .type = dots::simulation::TickCommandType::ApplyInput,
        .input_id = dots::simulation::InputCommandId{4},
        .owner_id = dots::simulation::PlayerOwnerId{3},
        .movement = {1.0F, 0.0F},
        .split_requested = true,
    }});
    REQUIRE(std::holds_alternative<dots::simulation::TickJournal>(tick));
    REQUIRE(world.player_count() == 2);

    dots::protocol::RecipientSessionState recipient{
        .mode = dots::protocol::SessionMode::Playing,
        .owned_entity_ids = {},
        .primary_entity_id = dots::replication::to_protocol(*player),
    };
    for (const auto player_id : world.player_ids()) {
        recipient.owned_entity_ids.push_back(dots::replication::to_protocol(player_id));
    }
    std::sort(recipient.owned_entity_ids.begin(), recipient.owned_entity_ids.end());
    const auto built = dots::replication::build_full_snapshot(world,
                                                              dots::protocol::SnapshotId{7},
                                                              dots::protocol::InputSequenceId{4},
                                                              0,
                                                              std::move(recipient));
    const auto* snapshot = std::get_if<dots::protocol::FullSnapshot>(&built);
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->entities.size() == 3);
    REQUIRE(snapshot->owners.size() == 1);
    REQUIRE(std::any_of(snapshot->entities.begin(),
                        snapshot->entities.end(),
                        [](const dots::protocol::EntityState& entity) {
                            return entity.prediction_key.has_value();
                        }));

    const auto rules = dots::replication::to_protocol(world.rules());
    const auto hydrated = dots::replication::hydrate_checkpoint(*snapshot, rules);
    const auto* checkpoint = std::get_if<dots::simulation::WorldCheckpoint>(&hydrated);
    REQUIRE(checkpoint != nullptr);
    CHECK(*checkpoint == world.checkpoint());

    auto invalid = *snapshot;
    invalid.checkpoint_digest ^= 1U;
    CHECK(std::get<dots::replication::CheckpointHydrationError>(
              dots::replication::hydrate_checkpoint(invalid, rules)) ==
          dots::replication::CheckpointHydrationError::DigestMismatch);

    invalid = *snapshot;
    const auto food_entity = std::find_if(
        invalid.entities.begin(), invalid.entities.end(), [](const dots::protocol::EntityState& e) {
            return e.kind == dots::protocol::EntityKind::Food;
        });
    REQUIRE(food_entity != invalid.entities.end());
    food_entity->mass += 1.0F;
    CHECK(std::get<dots::replication::CheckpointHydrationError>(
              dots::replication::hydrate_checkpoint(invalid, rules)) ==
          dots::replication::CheckpointHydrationError::InvalidFoodMass);

    auto incompatible_rules = rules;
    incompatible_rules.player_speed_units_per_second += 1.0F;
    CHECK(std::get<dots::replication::CheckpointHydrationError>(
              dots::replication::hydrate_checkpoint(*snapshot, incompatible_rules)) ==
          dots::replication::CheckpointHydrationError::DigestMismatch);
}

TEST_CASE("Replicated worlds accept contiguous receipts and reject gaps or conflicts atomically",
          "[dots][replication][receipts]") {
    dots::simulation::World world;
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    REQUIRE(player.has_value());
    auto built = dots::replication::build_full_snapshot(
        world,
        dots::protocol::SnapshotId{1},
        dots::protocol::InputSequenceId::invalid(),
        0,
        playing_session(dots::replication::to_protocol(*player)),
        {{
            .sequence_id = dots::protocol::AuthorityReceiptSequenceId{0},
            .event =
                dots::protocol::FoodConsumed{
                    .server_tick = 0,
                    .food_entity_id = dots::protocol::EntityId{20},
                    .consumer_entity_id = dots::replication::to_protocol(*player),
                    .consumer_owner_id = dots::protocol::PlayerOwnerId{0},
                    .transferred_mass = 1.0F,
                },
        }});
    auto* first = std::get_if<dots::protocol::FullSnapshot>(&built);
    REQUIRE(first != nullptr);

    dots::replication::ReplicatedWorld replicated;
    REQUIRE(replicated.apply(*first) == dots::replication::SnapshotApplyResult::Applied);
    CHECK(replicated.authority_receipt_acknowledgement() ==
          dots::protocol::AuthorityReceiptSequenceId{0});
    REQUIRE(replicated.authority_receipts().size() == 1);

    auto second = *first;
    second.snapshot_id = dots::protocol::SnapshotId{2};
    second.authority_receipts.push_back({
        .sequence_id = dots::protocol::AuthorityReceiptSequenceId{1},
        .event =
            dots::protocol::PlayerSplit{
                .server_tick = 0,
                .owner_id = dots::protocol::PlayerOwnerId{0},
                .input_id = dots::protocol::InputSequenceId{4},
                .child_ordinal = 0,
                .parent_entity_id = dots::replication::to_protocol(*player),
                .child_entity_id = dots::protocol::EntityId{21},
                .parent_mass = 8.0F,
                .child_mass = 8.0F,
            },
    });
    REQUIRE(replicated.apply(second) == dots::replication::SnapshotApplyResult::Applied);
    CHECK(replicated.authority_receipt_acknowledgement() ==
          dots::protocol::AuthorityReceiptSequenceId{1});
    REQUIRE(replicated.authority_receipts().size() == 2);

    auto conflict = second;
    conflict.snapshot_id = dots::protocol::SnapshotId{3};
    std::get<dots::protocol::FoodConsumed>(conflict.authority_receipts[0].event).transferred_mass =
        2.0F;
    CHECK(replicated.apply(conflict) == dots::replication::SnapshotApplyResult::Invalid);
    CHECK(replicated.snapshot_id() == dots::protocol::SnapshotId{2});

    auto gap = second;
    gap.snapshot_id = dots::protocol::SnapshotId{3};
    gap.authority_receipts = {{
        .sequence_id = dots::protocol::AuthorityReceiptSequenceId{3},
        .event = second.authority_receipts[1].event,
    }};
    CHECK(replicated.apply(gap) == dots::replication::SnapshotApplyResult::Invalid);
    CHECK(replicated.snapshot_id() == dots::protocol::SnapshotId{2});
}

TEST_CASE("Authority events round-trip through protocol receipts",
          "[dots][replication][receipts]") {
    const std::array<dots::simulation::SimulationEvent, 4> events{
        dots::simulation::FoodConsumed{
            .tick = mycore::time::Tick{3},
            .food_entity_id = dots::simulation::EntityId{10},
            .consumer_entity_id = dots::simulation::EntityId{11},
            .consumer_owner_id = dots::simulation::PlayerOwnerId{1},
            .transferred_mass = 1.0F,
        },
        dots::simulation::PlayerAbsorbed{
            .tick = mycore::time::Tick{4},
            .absorber_entity_id = dots::simulation::EntityId{11},
            .victim_entity_id = dots::simulation::EntityId{12},
            .absorber_owner_id = dots::simulation::PlayerOwnerId{1},
            .victim_owner_id = dots::simulation::PlayerOwnerId{2},
            .transferred_mass = 8.0F,
        },
        dots::simulation::PlayerSplit{
            .tick = mycore::time::Tick{5},
            .owner_id = dots::simulation::PlayerOwnerId{1},
            .input_id = dots::simulation::InputCommandId{6},
            .child_ordinal = 0,
            .parent_entity_id = dots::simulation::EntityId{11},
            .child_entity_id = dots::simulation::EntityId{13},
            .parent_mass = 8.0F,
            .child_mass = 8.0F,
        },
        dots::simulation::PiecesMerged{
            .tick = mycore::time::Tick{6},
            .owner_id = dots::simulation::PlayerOwnerId{1},
            .survivor_entity_id = dots::simulation::EntityId{11},
            .consumed_entity_id = dots::simulation::EntityId{13},
            .combined_mass = 16.0F,
        },
    };

    for (const auto& event : events) {
        const auto encoded = dots::replication::to_protocol(event);
        const auto* protocol_event = std::get_if<dots::protocol::AuthorityEvent>(&encoded);
        REQUIRE(protocol_event != nullptr);
        CHECK(dots::replication::to_simulation(*protocol_event) == event);
    }
}

TEST_CASE("Replicated worlds replace newer state and reject stale snapshots",
          "[dots][replication]") {
    dots::replication::ReplicatedWorld world;
    const dots::protocol::FullSnapshot first{
        .snapshot_id = dots::protocol::SnapshotId{1},
        .server_tick = 2,
        .last_processed_input_id = dots::protocol::InputSequenceId{3},
        .pending_input_count = 2,
        .recipient = playing_session(dots::protocol::EntityId{9}),
        .owners = {{
            .owner_id = dots::protocol::PlayerOwnerId{4},
        }},
        .entities =
            {
                {
                    .entity_id = dots::protocol::EntityId{2},
                    .kind = dots::protocol::EntityKind::Food,
                    .mass = 1.0F,
                },
                {
                    .entity_id = dots::protocol::EntityId{9},
                    .kind = dots::protocol::EntityKind::Player,
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                    .position_x = 4.0F,
                    .mass = 16.0F,
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
        .recipient = playing_session(dots::protocol::EntityId{12}),
        .owners = {{
            .owner_id = dots::protocol::PlayerOwnerId{5},
        }},
        .entities = {{
            .entity_id = dots::protocol::EntityId{12},
            .kind = dots::protocol::EntityKind::Player,
            .owner_id = dots::protocol::PlayerOwnerId{5},
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
        .recipient =
            {
                .mode = dots::protocol::SessionMode::Spectating,
                .defeat_tick = 0,
                .respawn_available_tick = 0,
            },
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
