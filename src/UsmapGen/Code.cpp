#include "UsmapGen/Code.h"

#include <algorithm>

#include <Zydis/Register.h>
#include <Zydis/Utils.h>

namespace UsmapGen {

namespace {

constexpr std::size_t maxInstructions = 0x40000;
constexpr int maxThunks = 4;
constexpr int maxThunkSetup = 2;
constexpr std::int64_t stackBelow = 0x4000;
constexpr std::int64_t stackAbove = 0x200;
constexpr std::size_t stackSlots = (stackBelow + stackAbove) / 8;
constexpr int registerCount = 16;
constexpr int vectorCount = 32;
constexpr std::size_t vectorLanes = 8;
constexpr std::size_t lowLanes = 2;
constexpr int nonvolatileVectorsBegin = 6;
constexpr int nonvolatileVectorsEnd = 16;
constexpr int rcx = 1;
constexpr int rdx = 2;
constexpr int rsp = 4;
constexpr int r8 = 8;
constexpr int r9 = 9;
constexpr std::array<int, 4> argumentRegisters = {rcx, rdx, r8, r9};
constexpr std::array<int, 7> volatileRegisters = {0, rcx, rdx, r8, r9, 10, 11};

constexpr Value unknown{};

using Lanes = std::array<Value, vectorLanes>;

constexpr Value constant(std::uint64_t value) { return {ValueKind::Constant, value}; }

constexpr std::uint64_t maskOf(std::uint32_t bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }

int vectorIndexOf(ZydisRegister reg) {
    const ZydisRegisterClass registerClass = ZydisRegisterGetClass(reg);
    if (registerClass != ZYDIS_REGCLASS_XMM && registerClass != ZYDIS_REGCLASS_YMM && registerClass != ZYDIS_REGCLASS_ZMM)
        return -1;
    return ZydisRegisterGetId(reg);
}

int indexOf(ZydisRegister reg) {
    const ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (full < ZYDIS_REGISTER_RAX || full > ZYDIS_REGISTER_R15)
        return -1;
    return full - ZYDIS_REGISTER_RAX;
}

bool isHighByte(ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH;
}

class Machine {
public:
    void clearVolatile() {
        for (const int reg : volatileRegisters)
            registers[reg] = unknown;
        for (int vector = 0; vector < vectorCount; vector++) {
            const bool keepsLow = vector >= nonvolatileVectorsBegin && vector < nonvolatileVectorsEnd;
            std::fill(vectors[vector].begin() + (keepsLow ? lowLanes : 0), vectors[vector].end(), unknown);
        }
    }

    Value get(int reg) const {
        if (reg == rsp)
            return {ValueKind::StackAddress, std::uint64_t(stackPointer)};
        return registers[reg];
    }

    void set(int reg, Value value) {
        if (reg != rsp)
            registers[reg] = value;
    }

    void moveStack(std::int64_t delta) { stackPointer += delta; }

    const Lanes& getVector(int vector) const { return vectors[vector]; }

    void setVector(int vector, const Lanes& lanes) { vectors[vector] = lanes; }

    void zeroUpperVectors(std::size_t firstLane) {
        for (Lanes& lanes : vectors)
            std::fill(lanes.begin() + firstLane, lanes.end(), constant(0));
    }

    std::optional<std::int64_t> stackOffsetOf(const ZydisDecodedOperandMem& memory) const {
        if (memory.index != ZYDIS_REGISTER_NONE)
            return std::nullopt;
        const int base = indexOf(memory.base);
        if (base < 0)
            return std::nullopt;
        const Value address = get(base);
        if (address.kind != ValueKind::StackAddress)
            return std::nullopt;
        return std::int64_t(address.value) + memory.disp.value;
    }

    void store(std::int64_t offset, std::uint32_t bytes, Value value) {
        const bool aligned = offset % 8 == 0 && (bytes == 8 || bytes == 4);
        for (std::int64_t at = offset & ~std::int64_t(7); at < offset + bytes; at += 8)
            if (const std::optional<std::size_t> slot = slotOf(at))
                stack[*slot] = aligned && at == offset ? value : unknown;
    }

    Value load(std::int64_t offset) const {
        const std::optional<std::size_t> slot = slotOf(offset);
        return slot ? stack[*slot] : unknown;
    }

    Call makeCall(std::uint64_t target) const {
        Call call;
        call.target = target;
        for (std::size_t i = 0; i < argumentRegisters.size(); i++)
            call.arguments[i] = registers[argumentRegisters[i]];
        for (std::size_t i = argumentRegisters.size(); i < Call::maxArguments; i++)
            call.arguments[i] = load(stackPointer + 0x20 + 8 * std::int64_t(i - argumentRegisters.size()));
        return call;
    }

private:
    static std::optional<std::size_t> slotOf(std::int64_t offset) {
        if (offset % 8 != 0 || offset < -stackBelow || offset >= stackAbove)
            return std::nullopt;
        return std::size_t((offset + stackBelow) / 8);
    }

    std::array<Value, registerCount> registers{};
    std::array<Value, stackSlots> stack{};
    std::int64_t stackPointer = 0;
    std::array<Lanes, vectorCount> vectors{};
};

Value addressOf(const Machine& machine, const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand& operand,
                std::uint64_t address) {
    const ZydisDecodedOperandMem& memory = operand.mem;
    if (memory.index != ZYDIS_REGISTER_NONE)
        return unknown;
    if (memory.base == ZYDIS_REGISTER_RIP)
        return constant(address + instruction.length + memory.disp.value);
    if (memory.base == ZYDIS_REGISTER_NONE)
        return constant(std::uint64_t(memory.disp.value));
    const int base = indexOf(memory.base);
    if (base < 0)
        return unknown;
    Value value = machine.get(base);
    if (value.kind == ValueKind::Unknown)
        return unknown;
    value.value += memory.disp.value;
    return value;
}

Value sourceOf(const Machine& machine, const ZydisDecodedOperand& source) {
    if (source.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
        return constant(source.imm.value.u);
    if (source.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        const std::optional<std::int64_t> offset = machine.stackOffsetOf(source.mem);
        if (!offset || source.size != 64)
            return unknown;
        return machine.load(*offset);
    }
    if (source.type != ZYDIS_OPERAND_TYPE_REGISTER)
        return unknown;
    const int reg = indexOf(source.reg.value);
    if (reg < 0 || isHighByte(source.reg.value))
        return unknown;
    Value value = machine.get(reg);
    if (value.kind == ValueKind::StackAddress && source.size != 64)
        return unknown;
    if (value.kind == ValueKind::Constant)
        value.value &= maskOf(source.size);
    return value;
}

void writeRegister(Machine& machine, const ZydisDecodedOperand& destination, Value value) {
    const int reg = indexOf(destination.reg.value);
    if (reg < 0)
        return;
    //! A write to an 8-bit or 16-bit register keeps the upper bits; a 32-bit write zero-extends.
    if (destination.size < 32 || (value.kind == ValueKind::StackAddress && destination.size != 64)) {
        machine.set(reg, unknown);
        return;
    }
    if (value.kind == ValueKind::Constant)
        value.value &= maskOf(destination.size);
    machine.set(reg, value);
}

void writeMemory(Machine& machine, const ZydisDecodedOperand& destination, Value value) {
    const std::optional<std::int64_t> offset = machine.stackOffsetOf(destination.mem);
    if (!offset)
        return;
    if (value.kind == ValueKind::Constant)
        value.value &= maskOf(destination.size);
    machine.store(*offset, destination.size / 8, value);
}

std::optional<Store> constantStoreOf(const Image& image, const Machine& machine, const ZydisDecodedInstruction& instruction,
                                     const ZydisDecodedOperand& destination, std::uint64_t address, Value value) {
    const Value target = addressOf(machine, instruction, destination, address);
    if (target.kind != ValueKind::Constant || value.kind != ValueKind::Constant || destination.size > 64 || !image.isWritableData(target.value))
        return std::nullopt;
    return Store{target.value, value.value & maskOf(destination.size), std::uint8_t(destination.size / 8)};
}

void clobber(Machine& machine, std::span<const ZydisDecodedOperand> operands) {
    for (const ZydisDecodedOperand& operand : operands) {
        if (!(operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
            continue;
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int reg = indexOf(operand.reg.value);
            if (reg >= 0)
                machine.set(reg, unknown);
            const int vector = vectorIndexOf(operand.reg.value);
            if (vector >= 0)
                machine.setVector(vector, Lanes{});
            continue;
        }
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY)
            writeMemory(machine, operand, unknown);
    }
}

Value maskLane(Value lane, std::uint32_t bits) {
    if (bits >= 64)
        return lane;
    if (lane.kind != ValueKind::Constant)
        return unknown;
    lane.value &= maskOf(bits);
    return lane;
}

std::size_t lanesIn(std::uint32_t bits) { return (bits + 63) / 64; }

Lanes lanesOf(Image& image, const Machine& machine, const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand& source,
              std::uint64_t address) {
    Lanes lanes{};
    if (source.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int vector = vectorIndexOf(source.reg.value);
        if (vector >= 0)
            return machine.getVector(vector);
        lanes[0] = sourceOf(machine, source);
        return lanes;
    }
    if (source.type != ZYDIS_OPERAND_TYPE_MEMORY)
        return lanes;
    const std::size_t count = lanesIn(source.size);
    if (const std::optional<std::int64_t> offset = machine.stackOffsetOf(source.mem)) {
        for (std::size_t i = 0; i < count; i++)
            lanes[i] = maskLane(machine.load(*offset + 8 * std::int64_t(i)), source.size - 64 * std::uint32_t(i));
        return lanes;
    }
    const Value at = addressOf(machine, instruction, source, address);
    const std::uint64_t bytes = source.size / 8;
    if (at.kind != ValueKind::Constant || !image.isReadOnlyData(at.value) || !image.isReadOnlyData(at.value + bytes - 1))
        return lanes;
    for (std::size_t i = 0; i < count; i++) {
        std::uint64_t lane = 0;
        image.read(at.value + 8 * i, std::as_writable_bytes(std::span(&lane, 1)).first(std::min<std::size_t>(8, bytes - 8 * i)));
        lanes[i] = constant(lane);
    }
    return lanes;
}

//! A VEX or EVEX write to a vector register zeroes the bits above it; a legacy SSE write keeps the bits above 128.
void writeVector(Machine& machine, const ZydisDecodedInstruction& instruction, int vector, std::uint32_t destinationBits,
                 std::uint32_t sourceBits, const Lanes& lanes) {
    Lanes result = machine.getVector(vector);
    const std::size_t written = lanesIn(sourceBits);
    const std::size_t cleared = instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY ? std::max(lanesIn(destinationBits), lowLanes) : vectorLanes;
    for (std::size_t i = 0; i < vectorLanes; i++) {
        if (i < written)
            result[i] = maskLane(lanes[i], sourceBits - 64 * std::uint32_t(i));
        else if (i < cleared)
            result[i] = constant(0);
    }
    machine.setVector(vector, result);
}

bool isFullVectorMove(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_VMOVUPS:
    case ZYDIS_MNEMONIC_VMOVUPD:
    case ZYDIS_MNEMONIC_VMOVAPS:
    case ZYDIS_MNEMONIC_VMOVAPD:
    case ZYDIS_MNEMONIC_VMOVDQU:
    case ZYDIS_MNEMONIC_VMOVDQA:
    case ZYDIS_MNEMONIC_VMOVDQU8:
    case ZYDIS_MNEMONIC_VMOVDQU16:
    case ZYDIS_MNEMONIC_VMOVDQU32:
    case ZYDIS_MNEMONIC_VMOVDQU64:
    case ZYDIS_MNEMONIC_VMOVDQA32:
    case ZYDIS_MNEMONIC_VMOVDQA64:
        return true;
    default:
        return false;
    }
}

bool isLowVectorMove(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_MOVSS:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_MOVD:
    case ZYDIS_MNEMONIC_MOVQ:
    case ZYDIS_MNEMONIC_VMOVSS:
    case ZYDIS_MNEMONIC_VMOVSD:
    case ZYDIS_MNEMONIC_VMOVD:
    case ZYDIS_MNEMONIC_VMOVQ:
        return true;
    default:
        return false;
    }
}

bool moveVector(Image& image, Machine& machine, const ZydisDecodedInstruction& instruction, std::span<const ZydisDecodedOperand> operands,
                std::uint64_t address, std::vector<Store>& stores) {
    if (operands.size() != 2)
        return false;
    const ZydisDecodedOperand& destination = operands[0];
    const ZydisDecodedOperand& source = operands[1];
    const bool fromVector = source.type == ZYDIS_OPERAND_TYPE_REGISTER && vectorIndexOf(source.reg.value) >= 0;
    const int vector = destination.type == ZYDIS_OPERAND_TYPE_REGISTER ? vectorIndexOf(destination.reg.value) : -1;
    if (!fromVector && vector < 0)
        return false;
    if (fromVector && vector >= 0 && !isFullVectorMove(instruction.mnemonic))
        return false;
    const Lanes lanes = lanesOf(image, machine, instruction, source, address);
    if (vector >= 0) {
        writeVector(machine, instruction, vector, destination.size, source.size, lanes);
        return true;
    }
    if (destination.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        writeRegister(machine, destination, lanes[0]);
        return true;
    }
    for (std::size_t i = 0; i < lanesIn(destination.size); i++) {
        const std::uint32_t bits = std::min<std::uint32_t>(64, destination.size - 64 * std::uint32_t(i));
        ZydisDecodedOperand part = destination;
        part.size = std::uint16_t(bits);
        part.mem.disp.value += 8 * std::int64_t(i);
        const Value lane = maskLane(lanes[i], bits);
        writeMemory(machine, part, lane);
        if (const std::optional<Store> store = constantStoreOf(image, machine, instruction, part, address, lane))
            stores.push_back(*store);
    }
    return true;
}

bool setsConstant(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands) {
    if (instruction.operand_count_visible != 2 || operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER)
        return false;
    if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV)
        return operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
    return instruction.mnemonic == ZYDIS_MNEMONIC_XOR && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[1].reg.value == operands[0].reg.value;
}

}

std::optional<std::uint64_t> Call::getConstant(const std::size_t index) const {
    const Value& value = arguments[index];
    if (value.kind != ValueKind::Constant)
        return std::nullopt;
    return value.value;
}

Code::Code(Image& image) : image(image) {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
}

bool Code::decode(const std::uint64_t address) {
    const std::uint64_t codeEnd = image.getCode().address + image.getCode().virtualSize;
    if (!image.isCode(address))
        return false;
    std::byte bytes[ZYDIS_MAX_INSTRUCTION_LENGTH];
    const std::size_t length = std::size_t(std::min<std::uint64_t>(sizeof(bytes), codeEnd - address));
    image.read(address, std::span(bytes, length));
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, length, &instruction, operands));
}

std::optional<std::uint64_t> Code::branchTarget(const std::uint64_t address) const {
    ZyanU64 target = 0;
    if (instruction.operand_count_visible == 0 || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
        !ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operands[0], address, &target)) || !image.isCode(target))
        return std::nullopt;
    return target;
}

//! Follows the control flow from the entry, so a function split into fragments or with code after a return is read whole.
void Code::traverse(const std::uint64_t function) {
    covered.clear();
    pending.assign(1, function);
    std::size_t budget = maxInstructions;
    while (!pending.empty() && budget > 0) {
        std::uint64_t address = pending.back();
        pending.pop_back();
        const auto next = covered.upper_bound(address);
        if (next != covered.begin() && std::prev(next)->second > address)
            continue;
        const std::uint64_t limit = next == covered.end() ? ~0ull : next->first;
        const std::uint64_t begin = address;
        while (address < limit && budget > 0 && decode(address)) {
            budget--;
            const std::uint64_t at = address;
            address += instruction.length;
            if (instruction.mnemonic == ZYDIS_MNEMONIC_RET || instruction.mnemonic == ZYDIS_MNEMONIC_INT3 || instruction.mnemonic == ZYDIS_MNEMONIC_UD2)
                break;
            const std::optional<std::uint64_t> target = branchTarget(at);
            if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
                if (target && (*target == function || !image.isFunctionStart(*target)))
                    pending.push_back(*target);
                break;
            }
            if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR && target)
                pending.push_back(*target);
        }
        if (address > begin)
            covered.emplace(begin, address);
    }
}

std::span<const Call> Code::findCalls(const std::uint64_t function) {
    return analyze(function).calls;
}

std::span<const Store> Code::findStores(const std::uint64_t function) {
    return analyze(function).stores;
}

//! An inlined append computes index * 5, loads the array, then writes five qwords at scale 8; separate pointer tracking leaves call/store interpretation unchanged.
std::vector<std::array<std::uint64_t, 5>> Code::findPointerRecords(const std::uint64_t function) {
    using Registers = std::array<std::optional<std::uint64_t>, registerCount>;
    std::vector<std::array<std::uint64_t, 5>> records;
    traverse(function);
    //! Loops need a fixpoint; this recognizer only accepts forward control flow.
    for (const auto [begin, end] : covered)
        for (std::uint64_t address = begin; address < end && decode(address); address += instruction.length)
            if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR || instruction.mnemonic == ZYDIS_MNEMONIC_JMP)
                if (const auto target = branchTarget(address); target && *target <= address)
                    return records;
    Registers registers{};
    bool reachable = false;
    std::map<std::uint64_t, Registers> incoming;
    const auto merge = [](Registers& into, const Registers& from) {
        for (std::size_t i = 0; i < into.size(); i++)
            if (into[i] != from[i])
                into[i].reset();
    };
    const auto valueOf = [&](const ZydisDecodedOperand& operand) -> std::optional<std::uint64_t> {
        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            return operand.imm.value.u;
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER && (operand.size == 64 || operand.size == 32)) {
            const int reg = indexOf(operand.reg.value);
            if (reg >= 0 && registers[reg])
                return operand.size == 32 ? std::uint32_t(*registers[reg]) : *registers[reg];
        }
        return std::nullopt;
    };
    std::array<std::uint64_t, 5> record{};
    std::size_t stage = 0;
    ZydisRegister index = ZYDIS_REGISTER_NONE, base = ZYDIS_REGISTER_NONE;
    std::uint64_t previousEnd = 0;
    for (const auto [begin, end] : covered) {
        if (begin != previousEnd) {
            registers = {};
            reachable = begin == function;
            stage = 0;
        }
        previousEnd = end;
        for (std::uint64_t address = begin; address < end && decode(address); address += instruction.length) {
            if (const auto edge = incoming.find(address); edge != incoming.end()) {
                //! A jump has no fallthrough predecessor; unknown values on a reachable path still participate in the merge.
                if (reachable)
                    merge(registers, edge->second);
                else
                    registers = edge->second;
                reachable = true;
                stage = 0;
            }
            if (!reachable)
                continue;
            const bool pair = instruction.operand_count_visible == 2;
            const auto& destination = operands[0];
            const auto& source = operands[1];
            if (stage == 1 && pair && instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
                destination.type == ZYDIS_OPERAND_TYPE_REGISTER && destination.size == 64 &&
                indexOf(destination.reg.value) >= 0 && destination.reg.value != index && destination.reg.value != ZYDIS_REGISTER_RSP &&
                source.type == ZYDIS_OPERAND_TYPE_MEMORY && source.size == 64 && indexOf(source.mem.base) >= 0 &&
                source.mem.base != ZYDIS_REGISTER_RSP && source.mem.index == ZYDIS_REGISTER_NONE && source.mem.disp.value == 0 &&
                (source.mem.segment == ZYDIS_REGISTER_DS || source.mem.segment == ZYDIS_REGISTER_SS)) {
                base = destination.reg.value;
                stage++;
            } else if (stage >= 2 && pair && instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
                       destination.type == ZYDIS_OPERAND_TYPE_MEMORY && destination.size == 64 &&
                       destination.mem.base == base && destination.mem.index == index && destination.mem.scale == 8 &&
                       (destination.mem.segment == ZYDIS_REGISTER_DS || destination.mem.segment == ZYDIS_REGISTER_SS) &&
                       destination.mem.disp.value == std::int64_t((stage - 2) * 8) && valueOf(source)) {
                record[stage - 2] = *valueOf(source);
                if (++stage == 7) {
                    records.push_back(record);
                    stage = 0;
                }
            } else {
                stage = 0;
                if (pair && instruction.mnemonic == ZYDIS_MNEMONIC_LEA && destination.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    destination.size == 64 && indexOf(destination.reg.value) >= 0 && destination.reg.value != ZYDIS_REGISTER_RSP &&
                    source.type == ZYDIS_OPERAND_TYPE_MEMORY && indexOf(source.mem.base) >= 0 &&
                    source.mem.base != ZYDIS_REGISTER_RSP && source.mem.base == source.mem.index &&
                    source.mem.scale == 4 && source.mem.disp.value == 0) {
                    index = destination.reg.value;
                    stage = 1;
                }
            }
            if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR || instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
                if (const auto target = branchTarget(address)) {
                    const auto [edge, inserted] = incoming.emplace(*target, registers);
                    if (!inserted)
                        merge(edge->second, registers);
                }
            }
            if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL) {
                for (const int reg : volatileRegisters)
                    registers[reg].reset();
                continue;
            }
            if (instruction.mnemonic == ZYDIS_MNEMONIC_JMP || instruction.mnemonic == ZYDIS_MNEMONIC_RET ||
                instruction.mnemonic == ZYDIS_MNEMONIC_INT3 || instruction.mnemonic == ZYDIS_MNEMONIC_UD2) {
                reachable = false;
                stage = 0;
                continue;
            }
            std::optional<std::uint64_t> value;
            if (pair && destination.type == ZYDIS_OPERAND_TYPE_REGISTER && (destination.size == 64 || destination.size == 32)) {
                if (instruction.mnemonic == ZYDIS_MNEMONIC_XOR && source.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    source.reg.value == destination.reg.value) {
                    value = 0;
                } else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV) {
                    value = valueOf(source);
                    if (source.type == ZYDIS_OPERAND_TYPE_MEMORY && (source.size == 64 || source.size == 32) &&
                        source.mem.base == ZYDIS_REGISTER_RIP && source.mem.index == ZYDIS_REGISTER_NONE &&
                        source.mem.segment == ZYDIS_REGISTER_DS) {
                        const std::uint64_t at = address + instruction.length + source.mem.disp.value;
                        if (image.isReadOnlyData(at) && image.isReadOnlyData(at + source.size / 8 - 1))
                            value = source.size == 32 ? image.read<std::uint32_t>(at) : image.read<std::uint64_t>(at);
                    }
                } else if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA && source.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                           source.mem.base == ZYDIS_REGISTER_RIP && source.mem.index == ZYDIS_REGISTER_NONE) {
                    value = address + instruction.length + source.mem.disp.value;
                }
                //! Every 32-bit GPR write zero-extends; narrower writes are only clobbers below.
                if (value && destination.size == 32)
                    value = std::uint32_t(*value);
            }
            for (const auto& operand : std::span(operands, instruction.operand_count))
                if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER && (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
                    if (const int reg = indexOf(operand.reg.value); reg >= 0)
                        registers[reg].reset();
            if (value && destination.type == ZYDIS_OPERAND_TYPE_REGISTER)
                if (const int reg = indexOf(destination.reg.value); reg >= 0)
                    registers[reg] = value;
        }
    }
    return records;
}

const References& Code::findReferences(const std::uint64_t function) {
    return analyze(function).references;
}

const Analysis& Code::analyze(const std::uint64_t function) {
    if (const auto found = analyses.find(function); found != analyses.end())
        return found->second;
    Analysis analysis;
    std::vector<Call>& calls = analysis.calls;
    std::vector<Store>& stores = analysis.stores;
    References& references = analysis.references;
    traverse(function);
    Machine machine;
    std::uint64_t previousEnd = 0;
    for (const auto [begin, end] : covered) {
        if (begin != previousEnd)
            machine = Machine();
        previousEnd = end;
        for (std::uint64_t address = begin; address < end && decode(address); address += instruction.length) {
            const std::span<const ZydisDecodedOperand> visible(operands, instruction.operand_count_visible);
            switch (instruction.mnemonic) {
            case ZYDIS_MNEMONIC_CALL:
            case ZYDIS_MNEMONIC_JMP: {
                const std::optional<std::uint64_t> target = branchTarget(address);
                const bool call = instruction.mnemonic == ZYDIS_MNEMONIC_CALL;
                if (target && (call || image.isFunctionStart(*target))) {
                    calls.push_back(machine.makeCall(*target));
                    references.code.push_back(*target);
                }
                if (call)
                    machine.clearVolatile();
                continue;
            }
            default:
                break;
            }
            for (const ZydisDecodedOperand& operand : visible) {
                if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.base != ZYDIS_REGISTER_RIP)
                    continue;
                const std::uint64_t target = address + instruction.length + std::uint64_t(operand.mem.disp.value);
                if (image.isData(target))
                    references.data.push_back(target);
                else if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA && image.isCode(target))
                    references.code.push_back(target);
            }
            switch (instruction.mnemonic) {
            case ZYDIS_MNEMONIC_PUSH:
                machine.moveStack(-8);
                continue;
            case ZYDIS_MNEMONIC_POP:
                machine.moveStack(8);
                clobber(machine, visible);
                continue;
            case ZYDIS_MNEMONIC_SUB:
            case ZYDIS_MNEMONIC_ADD:
            case ZYDIS_MNEMONIC_OR: {
                if (visible[0].type != ZYDIS_OPERAND_TYPE_REGISTER || visible[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
                    break;
                const std::uint64_t operand = visible[1].imm.value.u;
                if (indexOf(visible[0].reg.value) == rsp && instruction.mnemonic != ZYDIS_MNEMONIC_OR) {
                    machine.moveStack(instruction.mnemonic == ZYDIS_MNEMONIC_SUB ? -std::int64_t(operand) : std::int64_t(operand));
                    continue;
                }
                Value value = sourceOf(machine, visible[0]);
                if (value.kind != ValueKind::Constant)
                    break;
                value.value = instruction.mnemonic == ZYDIS_MNEMONIC_OR ? value.value | operand
                              : instruction.mnemonic == ZYDIS_MNEMONIC_ADD ? value.value + operand
                                                                           : value.value - operand;
                writeRegister(machine, visible[0], value);
                continue;
            }
            case ZYDIS_MNEMONIC_LEA:
                writeRegister(machine, visible[0], addressOf(machine, instruction, visible[1], address));
                continue;
            case ZYDIS_MNEMONIC_MOV: {
                const Value value = sourceOf(machine, visible[1]);
                if (visible[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    writeRegister(machine, visible[0], value);
                    continue;
                }
                writeMemory(machine, visible[0], value);
                if (const std::optional<Store> store = constantStoreOf(image, machine, instruction, visible[0], address, value))
                    stores.push_back(*store);
                continue;
            }
            case ZYDIS_MNEMONIC_XOR:
                if (visible[0].type != ZYDIS_OPERAND_TYPE_REGISTER || visible[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                    visible[0].reg.value != visible[1].reg.value)
                    break;
                writeRegister(machine, visible[0], constant(0));
                continue;
            case ZYDIS_MNEMONIC_XORPS:
            case ZYDIS_MNEMONIC_XORPD:
            case ZYDIS_MNEMONIC_PXOR:
            case ZYDIS_MNEMONIC_VXORPS:
            case ZYDIS_MNEMONIC_VXORPD:
            case ZYDIS_MNEMONIC_VPXOR:
            case ZYDIS_MNEMONIC_VPXORD:
            case ZYDIS_MNEMONIC_VPXORQ: {
                const int vector = visible[0].type == ZYDIS_OPERAND_TYPE_REGISTER ? vectorIndexOf(visible[0].reg.value) : -1;
                const bool zeroing = vector >= 0 && std::ranges::all_of(visible.subspan(1), [&](const ZydisDecodedOperand& source) {
                    return source.type == ZYDIS_OPERAND_TYPE_REGISTER && source.reg.value == visible[0].reg.value;
                });
                if (!zeroing)
                    break;
                Lanes zero;
                zero.fill(constant(0));
                writeVector(machine, instruction, vector, visible[0].size, visible[0].size, zero);
                continue;
            }
            case ZYDIS_MNEMONIC_VZEROUPPER:
                machine.zeroUpperVectors(lowLanes);
                continue;
            case ZYDIS_MNEMONIC_VZEROALL:
                machine.zeroUpperVectors(0);
                continue;
            case ZYDIS_MNEMONIC_AND:
                //! MSVC clears a stack argument with `and qword ptr [rsp+N], 0`.
                if (visible[0].type != ZYDIS_OPERAND_TYPE_MEMORY || visible[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                    visible[1].imm.value.u != 0)
                    break;
                writeMemory(machine, visible[0], constant(0));
                continue;
            default:
                if ((isFullVectorMove(instruction.mnemonic) || isLowVectorMove(instruction.mnemonic)) &&
                    moveVector(image, machine, instruction, visible, address, stores))
                    continue;
                break;
            }
            clobber(machine, std::span<const ZydisDecodedOperand>(operands, instruction.operand_count));
        }
    }
    return analyses.emplace(function, std::move(analysis)).first->second;
}

//! A thunk only sets registers to constants, as UE 5.8 StaticClass does with `xor ecx, ecx`, before it jumps on.
std::uint64_t Code::followThunks(std::uint64_t function) {
    for (int thunk = 0; thunk < maxThunks; thunk++) {
        std::uint64_t address = function;
        int setup = 0;
        while (decode(address) && instruction.mnemonic != ZYDIS_MNEMONIC_JMP && setup < maxThunkSetup && setsConstant(instruction, operands)) {
            address += instruction.length;
            setup++;
        }
        if (instruction.mnemonic != ZYDIS_MNEMONIC_JMP)
            return function;
        const std::optional<std::uint64_t> target = branchTarget(address);
        if (!target)
            return function;
        function = *target;
    }
    return function;
}

}
