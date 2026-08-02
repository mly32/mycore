#pragma once

#include "dots/prediction/types.hpp"
#include "dots/simulation/world_state.hpp"

#include <variant>

namespace dots::prediction {

using ScopeBuildResult = std::variant<PredictionScope, ScopeBuildError>;

// The complete public scope contract used by construction, projection, restore, and replay.
// Keeping one validator prevents malformed scopes from being accepted at one boundary only to
// fail later with a less actionable checkpoint or simulation error.
[[nodiscard]] bool is_valid_prediction_scope(const PredictionScope& scope) noexcept;

[[nodiscard]] ScopeBuildResult build_prediction_scope(const simulation::WorldCheckpoint& authority,
                                                      const PredictionRequest& request);

using CheckpointProjectionResult = std::variant<simulation::WorldCheckpoint, PredictionError>;

[[nodiscard]] CheckpointProjectionResult
project_checkpoint(const simulation::WorldCheckpoint& authority, const PredictionScope& scope);

} // namespace dots::prediction
