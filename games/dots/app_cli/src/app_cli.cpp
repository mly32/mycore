#include "dots/app_cli/app_cli.hpp"

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace dots::app_cli {

std::string_view
require_option_value(int& index, int argument_count, char** arguments, std::string_view option) {
    if (index + 1 >= argument_count) {
        throw ParseError{std::string{option} + " requires a value"};
    }
    return arguments[++index];
}

std::uint32_t parse_nonnegative_u32(std::string_view value, std::string_view option) {
    std::uint32_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw ParseError{std::string{option} + " requires a non-negative integer"};
    }
    return result;
}

std::uint64_t parse_positive_u64(std::string_view value, std::string_view option) {
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result == 0) {
        throw ParseError{std::string{option} + " requires a positive integer"};
    }
    return result;
}

float parse_percent(std::string_view value, std::string_view option) {
    float result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result) ||
        result < 0.0F || result > 100.0F) {
        throw ParseError{std::string{option} + " requires a number in the range 0..100"};
    }
    return result;
}

mycore::net_transport::NetworkAddress parse_connect_address(std::string_view value,
                                                            std::string_view option) {
    const auto address = mycore::net_transport::NetworkAddress::parse(value);
    if (!address || address->port() == 0) {
        throw ParseError{std::string{option} +
                         " requires a numeric IPv4 or bracketed IPv6 address with a port"};
    }
    return *address;
}

mycore::net_transport::NetworkAddress parse_listen_address(std::string_view value,
                                                           std::string_view option) {
    const auto address = mycore::net_transport::NetworkAddress::parse(value);
    if (!address) {
        throw ParseError{std::string{option} +
                         " requires a numeric IPv4 or bracketed IPv6 address"};
    }
    return *address;
}

bool consume_network_impairment_option(std::string_view argument,
                                       int& index,
                                       int argument_count,
                                       char** arguments,
                                       mycore::net_transport::NetworkImpairment& impairment) {
    if (argument == "--fake-lag-ms") {
        impairment.outgoing_lag_milliseconds = parse_nonnegative_u32(
            require_option_value(index, argument_count, arguments, argument), argument);
        return true;
    }
    if (argument == "--fake-loss-percent") {
        impairment.outgoing_loss_percent = parse_percent(
            require_option_value(index, argument_count, arguments, argument), argument);
        return true;
    }
    return false;
}

} // namespace dots::app_cli
