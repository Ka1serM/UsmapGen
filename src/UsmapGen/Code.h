#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
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

struct Store {
    std::uint64_t address;
    std::uint64_t value;
    std::uint8_t size;
};

struct References {
    std::vector<std::uint64_t> data;
    std::vector<std::uint64_t> code;
};

struct Analysis {
    std::vector<Call> calls;
    References references;
    std::vector<Store> stores;
};

class Code {
public:
    explicit Code(Image& image);
    Code(const Code&) = delete;
    Code& operator=(const Code&) = delete;

    std::span<const Call> findCalls(std::uint64_t function);
    std::span<const Store> findStores(std::uint64_t function);
    std::vector<std::array<std::uint64_t, 5>> findPointerRecords(std::uint64_t function);
    const References& findReferences(std::uint64_t function);
    std::uint64_t followThunks(std::uint64_t function);

private:
    bool decode(std::uint64_t address);
    std::optional<std::uint64_t> branchTarget(std::uint64_t address) const;
    void traverse(std::uint64_t function);
    const Analysis& analyze(std::uint64_t function);

    Image& image;
    ZydisDecoder decoder;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    std::map<std::uint64_t, std::uint64_t> covered;
    std::vector<std::uint64_t> pending;
    std::unordered_map<std::uint64_t, Analysis> analyses;
};

}
