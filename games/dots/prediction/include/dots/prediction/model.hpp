#pragma once

#include "dots/prediction/scope.hpp"
#include "dots/prediction/types.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/rollback/rollback.hpp"

#include <variant>
#include <vector>

namespace dots::prediction {

[[nodiscard]] StateDigest checkpoint_digest(const simulation::WorldCheckpoint& checkpoint);

class WorldModel {
public:
    using State = simulation::World;
    using Checkpoint = simulation::WorldCheckpoint;
    using Stimulus = TickStimulus;
    using Scope = PredictionScope;
    using Event = simulation::SimulationEvent;
    using EventKey = simulation::SimulationEventKey;
    using EventKeyHash = SimulationEventKeyHash;
    using StateDiff = StateDifference;
    using StateDigest = prediction::StateDigest;
    using Error = PredictionError;

    [[nodiscard]] std::variant<State, Error> restore(const Checkpoint& checkpoint,
                                                     const Scope& scope) const;
    [[nodiscard]] Checkpoint capture(const State& state, const Scope& scope) const;
    [[nodiscard]] std::variant<std::vector<Event>, Error>
    step(State& state, const Stimulus& stimulus, const Scope& scope) const;
    [[nodiscard]] StateDigest digest(const Checkpoint& checkpoint, const Scope& scope) const;
    [[nodiscard]] StateDiff
    diff(const State& previous, const State& current, const Scope& scope) const;
    [[nodiscard]] EventKey event_key(const Event& event) const;
};

static_assert(mycore::rollback::RollbackModel<WorldModel>);

using Timeline = mycore::rollback::Timeline<WorldModel>;
using AuthorityFrame = mycore::rollback::AuthorityFrame<WorldModel>;
using AuthorityEvent = mycore::rollback::AuthorityEvent<WorldModel>;
using Commit = mycore::rollback::Commit<WorldModel>;
using CommitResult = mycore::rollback::CommitResult<WorldModel>;
using TimelineFailure = mycore::rollback::TimelineFailure<WorldModel>;

} // namespace dots::prediction
