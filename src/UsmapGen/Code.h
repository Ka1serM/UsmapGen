#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <Zydis/Decoder.h>

#include "UsmapGen/Image.h"

namespace UsmapGen {

enum class ValueKind : std::uint8_t { Unknown, Constant, StackAddress };

struct Value {
    ValueKind kind = ValueKind::Unknown;
    std::uint64_t value = 0;
};

struct Call {
    static constexpr std::size_t maxArguments = 16;

    std::uint64_t target = 0;
    std::array<Value, maxArguments> arguments;

    std::optional<std::uint64_t> getConstant(std::size_t index) const;
};

class Code {
public:
    explicit Code(Image& image);
    Code(const Code&) = delete;
    Code& operator=(const Code&) = delete;

    std::span<const Call> findCalls(std::uint64_t function);
    std::uint64_t followThunks(std::uint64_t function);

private:
    Image& image;
    ZydisDecoder decoder;
    std::vector<std::byte> bytes;
    std::vector<Call> calls;
};

}
