#pragma once

#include "mycore/net_transport/net_transport.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace dots::app_cli {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string_view
require_option_value(int& index, int argument_count, char** arguments, std::string_view option);
[[nodiscard]] std::uint32_t parse_nonnegative_u32(std::string_view value, std::string_view option);
[[nodiscard]] std::uint64_t parse_positive_u64(std::string_view value, std::string_view option);
[[nodiscard]] float parse_percent(std::string_view value, std::string_view option);
[[nodiscard]] mycore::net_transport::NetworkAddress parse_connect_address(std::string_view value,
                                                                          std::string_view option);
[[nodiscard]] mycore::net_transport::NetworkAddress parse_listen_address(std::string_view value,
                                                                         std::string_view option);

// Returns true and consumes its value when argument is a common network impairment option.
[[nodiscard]] bool
consume_network_impairment_option(std::string_view argument,
                                  int& index,
                                  int argument_count,
                                  char** arguments,
                                  mycore::net_transport::NetworkImpairment& impairment);

} // namespace dots::app_cli
