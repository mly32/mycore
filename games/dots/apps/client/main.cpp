#include "dots/simulation/simulation.hpp"

#include <iostream>

int main() {
    std::cout << "dots_client: foundation ready\n";
    return dots::simulation::foundation_ready() ? 0 : 1;
}
