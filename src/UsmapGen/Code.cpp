#include "UsmapGen/Code.h"

#include <Zydis/Register.h>
#include <Zydis/Utils.h>

namespace UsmapGen {

namespace {

constexpr std::uint64_t leafFunctionLimit = 0x1000;
constexpr std::uint64_t functionLimit = 0x100000;
constexpr int maxThunks = 4;
constexpr std::int64_t stackBelow = 0x4000;
constexpr std::int64_t stackAbove = 0x200;
constexpr std::size_t stackSlots = (stackBelow + stackAbove) / 8;
constexpr int registerCount = 16;
constexpr int rcx = 1;
constexpr int rdx = 2;
constexpr int rsp = 4;
constexpr int r8 = 8;
constexpr int r9 = 9;
constexpr std::array<int, 4> argumentRegisters = {rcx, rdx, r8, r9};
constexpr std::array<int, 7> volatileRegisters = {0, rcx, rdx, r8, r9, 10, 11};

constexpr Value unknown{};

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

class Machine {
public:
    void clearVolatile() {
        for (const int reg : volatileRegisters)
            registers[reg] = unknown;
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

    bool isZeroVector(int vector) const { return zeroVectors >> vector & 1; }

    void setZeroVector(int vector, bool zero) {
        const std::uint32_t bit = std::uint32_t(1) << vector;
        zeroVectors = zero ? zeroVectors | bit : zeroVectors & ~bit;
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
    std::uint32_t zeroVectors = 0;
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
    if (reg < 0 || source.size < 32)
        return unknown;
    Value value = machine.get(reg);
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

bool isVectorZeroing(std::span<const ZydisDecodedOperand> operands) {
    if (operands.size() < 2 || operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER || vectorIndexOf(operands[0].reg.value) < 0)
        return false;
    for (const ZydisDecodedOperand& source : operands.subspan(1))
        if (source.type != ZYDIS_OPERAND_TYPE_REGISTER || source.reg.value != operands[0].reg.value)
            return false;
    return true;
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
                machine.setZeroVector(vector, false);
            continue;
        }
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY)
            writeMemory(machine, operand, unknown);
    }
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

std::uint64_t Code::followThunks(std::uint64_t function) {
    for (int i = 0; i < maxThunks && image.isCode(function); i++) {
        std::byte jump[5];
        image.read(function, jump);
        if (jump[0] != std::byte{0xE9})
            return function;
        std::int32_t displacement;
        std::memcpy(&displacement, jump + 1, 4);
        function += 5 + std::int64_t(displacement);
    }
    return function;
}

std::span<const Call> Code::findCalls(const std::uint64_t function) {
    calls.clear();
    if (!image.isCode(function))
        return calls;
    const std::uint64_t codeEnd = image.getCode().address + image.getCode().virtualSize;
    const std::uint64_t end = std::min(codeEnd, image.getFunctionEnd(function).value_or(function + leafFunctionLimit));
    if (end <= function || end - function > functionLimit)
        return calls;
    bytes.resize(end - function);
    image.read(function, bytes);

    Machine machine;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    for (std::size_t at = 0; at < bytes.size(); at += instruction.length) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + at, bytes.size() - at, &instruction, operands)))
            return calls;
        const std::uint64_t address = function + at;
        const std::span<const ZydisDecodedOperand> visible(operands, instruction.operand_count_visible);
        switch (instruction.mnemonic) {
        case ZYDIS_MNEMONIC_RET:
        case ZYDIS_MNEMONIC_INT3:
            return calls;
        case ZYDIS_MNEMONIC_CALL:
        case ZYDIS_MNEMONIC_JMP: {
            ZyanU64 target = 0;
            const bool direct = visible[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                                ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &visible[0], address, &target));
            if (direct)
                calls.push_back(machine.makeCall(target));
            if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
                machine.clearVolatile();
            continue;
        }
        case ZYDIS_MNEMONIC_PUSH:
            machine.moveStack(-8);
            continue;
        case ZYDIS_MNEMONIC_POP:
            machine.moveStack(8);
            clobber(machine, visible);
            continue;
        case ZYDIS_MNEMONIC_SUB:
        case ZYDIS_MNEMONIC_ADD: {
            const bool stack = visible[0].type == ZYDIS_OPERAND_TYPE_REGISTER && indexOf(visible[0].reg.value) == rsp &&
                               visible[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
            if (!stack)
                break;
            const std::int64_t delta = visible[1].imm.value.s;
            machine.moveStack(instruction.mnemonic == ZYDIS_MNEMONIC_SUB ? -delta : delta);
            continue;
        }
        case ZYDIS_MNEMONIC_LEA:
            writeRegister(machine, visible[0], addressOf(machine, instruction, visible[1], address));
            continue;
        case ZYDIS_MNEMONIC_MOV: {
            const Value value = sourceOf(machine, visible[1]);
            if (visible[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
                writeRegister(machine, visible[0], value);
            else
                writeMemory(machine, visible[0], value);
            continue;
        }
        case ZYDIS_MNEMONIC_XOR:
            if (visible[0].type != ZYDIS_OPERAND_TYPE_REGISTER || visible[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                visible[0].reg.value != visible[1].reg.value)
                break;
            writeRegister(machine, visible[0], constant(0));
            continue;
        case ZYDIS_MNEMONIC_XORPS:
        case ZYDIS_MNEMONIC_PXOR:
        case ZYDIS_MNEMONIC_VXORPS:
        case ZYDIS_MNEMONIC_VPXOR:
            if (!isVectorZeroing(visible))
                break;
            machine.setZeroVector(vectorIndexOf(visible[0].reg.value), true);
            continue;
        case ZYDIS_MNEMONIC_MOVUPS:
        case ZYDIS_MNEMONIC_MOVAPS:
        case ZYDIS_MNEMONIC_MOVDQU:
        case ZYDIS_MNEMONIC_MOVDQA:
        case ZYDIS_MNEMONIC_VMOVUPS:
        case ZYDIS_MNEMONIC_VMOVAPS:
        case ZYDIS_MNEMONIC_VMOVDQU:
        case ZYDIS_MNEMONIC_VMOVDQA: {
            //! Compilers clear several stack arguments at once with a store of a zeroed vector register.
            if (visible[0].type != ZYDIS_OPERAND_TYPE_MEMORY || visible[1].type != ZYDIS_OPERAND_TYPE_REGISTER)
                break;
            const std::optional<std::int64_t> offset = machine.stackOffsetOf(visible[0].mem);
            if (!offset)
                break;
            const bool zero = machine.isZeroVector(vectorIndexOf(visible[1].reg.value));
            for (std::uint32_t part = 0; part < visible[0].size / 64; part++)
                machine.store(*offset + 8 * part, 8, zero ? constant(0) : unknown);
            continue;
        }
        case ZYDIS_MNEMONIC_AND:
            //! MSVC clears a stack argument with `and qword ptr [rsp+N], 0`.
            if (visible[0].type != ZYDIS_OPERAND_TYPE_MEMORY || visible[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                visible[1].imm.value.u != 0)
                break;
            writeMemory(machine, visible[0], constant(0));
            continue;
        default:
            break;
        }
        clobber(machine, std::span<const ZydisDecodedOperand>(operands, instruction.operand_count));
    }
    return calls;
}

}
