#include "dots/app_cli/app_cli.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

TEST_CASE("Dots app CLI parses shared numeric values", "[dots][app-cli]") {
    CHECK(dots::app_cli::parse_nonnegative_u32("42", "--count") == 42);
    CHECK(dots::app_cli::parse_positive_u64("9001", "--ticks") == 9001);
    CHECK(dots::app_cli::parse_percent("12.5", "--loss") == Catch::Approx(12.5F));
    CHECK_THROWS_AS(dots::app_cli::parse_nonnegative_u32("-1", "--count"),
                    dots::app_cli::ParseError);
    CHECK_THROWS_AS(dots::app_cli::parse_positive_u64("0", "--ticks"), dots::app_cli::ParseError);
    CHECK_THROWS_AS(dots::app_cli::parse_percent("100.1", "--loss"), dots::app_cli::ParseError);
}

TEST_CASE("Dots app CLI distinguishes connect and listen addresses", "[dots][app-cli]") {
    CHECK(dots::app_cli::parse_connect_address("127.0.0.1:27020", "--connect").port() == 27020);
    CHECK(dots::app_cli::parse_listen_address("127.0.0.1:0", "--listen").port() == 0);
    CHECK_THROWS_AS(dots::app_cli::parse_connect_address("127.0.0.1:0", "--connect"),
                    dots::app_cli::ParseError);
}

TEST_CASE("Dots app CLI consumes shared network impairment options", "[dots][app-cli]") {
    char tool[] = "dots_tool";
    char lag_option[] = "--fake-lag-ms";
    char lag_value[] = "25";
    char loss_option[] = "--fake-loss-percent";
    char loss_value[] = "12.5";
    std::array arguments{tool, lag_option, lag_value, loss_option, loss_value};
    auto argument_index = 1;
    mycore::net_transport::NetworkImpairment impairment;
    REQUIRE(dots::app_cli::consume_network_impairment_option(
        arguments[static_cast<std::size_t>(argument_index)],
        argument_index,
        static_cast<int>(arguments.size()),
        arguments.data(),
        impairment));
    CHECK(argument_index == 2);
    CHECK(impairment.outgoing_lag_milliseconds == 25);

    ++argument_index;
    REQUIRE(dots::app_cli::consume_network_impairment_option(
        arguments[static_cast<std::size_t>(argument_index)],
        argument_index,
        static_cast<int>(arguments.size()),
        arguments.data(),
        impairment));
    CHECK(argument_index == 4);
    CHECK(impairment.outgoing_loss_percent == Catch::Approx(12.5F));
}
