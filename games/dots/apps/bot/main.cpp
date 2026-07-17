#include "dots/simulation/simulation.hpp"
#include "mycore/debug/log.hpp"

int main() {
    const mycore::debug::Runtime logging;
    mycore::debug::log_info("dots.bot", "Foundation ready");
    return dots::simulation::foundation_ready() ? 0 : 1;
}
