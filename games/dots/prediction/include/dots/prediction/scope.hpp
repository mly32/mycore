#pragma once

#include "dots/prediction/types.hpp"
#include "dots/simulation/world_state.hpp"

#include <variant>

namespace dots::prediction {

using ScopeBuildResult = std::variant<PredictionScope, ScopeBuildError>;

[[nodiscard]] ScopeBuildResult build_prediction_scope(const simulation::WorldCheckpoint& authority,
                                                      const PredictionRequest& request);

using CheckpointProjectionResult = std::variant<simulation::WorldCheckpoint, PredictionError>;

[[nodiscard]] CheckpointProjectionResult
project_checkpoint(const simulation::WorldCheckpoint& authority, const PredictionScope& scope);

} // namespace dots::prediction
