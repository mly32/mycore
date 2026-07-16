#include "dots/simulation/simulation.hpp"

#include "mycore/core/core.hpp"

namespace dots::simulation {

bool foundation_ready() noexcept {
    return mycore::core::library_name() == "MyCore::Core";
}

} // namespace dots::simulation
