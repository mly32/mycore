#pragma once

#include "mycore/core/strong_id.hpp"

#include <cstdint>

namespace dots::simulation {

struct EntityIdTag;
struct InputCommandIdTag;

using EntityId = mycore::core::StrongId<EntityIdTag, std::uint32_t>;
using InputCommandId = mycore::core::StrongId<InputCommandIdTag, std::uint32_t>;

} // namespace dots::simulation
