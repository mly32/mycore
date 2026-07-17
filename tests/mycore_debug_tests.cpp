#include "mycore/debug/metrics.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>

using Catch::Approx;
using namespace std::chrono_literals;

TEST_CASE("Frame metrics are empty by default") {
    const mycore::debug::FrameMetrics metrics{3};

    REQUIRE(metrics.capacity() == 3);
    const auto snapshot = metrics.snapshot();
    CHECK(snapshot.sample_count == 0);
    CHECK(snapshot.total_frame_count == 0);
    CHECK(snapshot.latest_milliseconds == 0.0);
    CHECK(snapshot.average_milliseconds == 0.0);
    CHECK(snapshot.frames_per_second == 0.0);
}

TEST_CASE("Frame metrics aggregate and evict a bounded rolling window") {
    mycore::debug::FrameMetrics metrics{3};
    metrics.add_sample(10ms);
    metrics.add_sample(20ms);
    metrics.add_sample(30ms);

    auto snapshot = metrics.snapshot();
    CHECK(snapshot.sample_count == 3);
    CHECK(snapshot.total_frame_count == 3);
    CHECK(snapshot.latest_milliseconds == Approx(30.0));
    CHECK(snapshot.average_milliseconds == Approx(20.0));
    CHECK(snapshot.minimum_milliseconds == Approx(10.0));
    CHECK(snapshot.maximum_milliseconds == Approx(30.0));
    CHECK(snapshot.frames_per_second == Approx(50.0));

    metrics.add_sample(40ms);
    snapshot = metrics.snapshot();
    CHECK(snapshot.sample_count == 3);
    CHECK(snapshot.total_frame_count == 4);
    CHECK(snapshot.average_milliseconds == Approx(30.0));
    CHECK(snapshot.minimum_milliseconds == Approx(20.0));
    CHECK(snapshot.maximum_milliseconds == Approx(40.0));
}

TEST_CASE("Frame metrics reset and reject invalid construction or samples") {
    CHECK_THROWS_AS(mycore::debug::FrameMetrics{0}, std::invalid_argument);

    mycore::debug::FrameMetrics metrics{2};
    CHECK_THROWS_AS(metrics.add_sample(-1ns), std::invalid_argument);
    metrics.add_sample(5ms);
    metrics.reset();

    const auto snapshot = metrics.snapshot();
    CHECK(snapshot.sample_count == 0);
    CHECK(snapshot.total_frame_count == 0);
    CHECK(snapshot.latest_milliseconds == 0.0);
}
