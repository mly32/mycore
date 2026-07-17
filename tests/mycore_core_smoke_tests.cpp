#include "mycore/core/core.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("MyCore Core reports its library name", "[core]") {
    REQUIRE(mycore::core::library_name() == "MyCore::Core");
}
