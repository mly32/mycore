#include "dots/simulation/simulation.hpp"
#include "mycore/core/core.hpp"

#include <iostream>

int main() {
    if (mycore::core::library_name() != "MyCore::Core") {
        std::cerr << "MyCore::Core did not return its expected identity\n";
        return 1;
    }

    if (!dots::simulation::foundation_ready()) {
        std::cerr << "Dots::Simulation did not link to MyCore::Core\n";
        return 1;
    }

    return 0;
}
