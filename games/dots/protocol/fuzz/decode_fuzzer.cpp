#include "dots/protocol/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::as_bytes(std::span{data, size});
    static_cast<void>(dots::protocol::decode(input));
    return 0;
}
