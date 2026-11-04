#include "UsmapGen/Code.h"

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
    calls.clear();
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
                if (target && (call || image.isFunctionStart(*target)))
                    calls.push_back(machine.makeCall(*target));
                if (call)
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
    }
    return calls;
}

const References& Code::findReferences(const std::uint64_t function) {
    if (const auto found = references.find(function); found != references.end())
        return found->second;
    References result;
    traverse(function);
    for (const auto [begin, end] : covered) {
        for (std::uint64_t address = begin; address < end && decode(address); address += instruction.length) {
            const bool call = instruction.mnemonic == ZYDIS_MNEMONIC_CALL;
            if (call || instruction.mnemonic == ZYDIS_MNEMONIC_JMP) {
                const std::optional<std::uint64_t> target = branchTarget(address);
                if (target && (call || image.isFunctionStart(*target)))
                    result.code.push_back(*target);
                continue;
            }
            for (const ZydisDecodedOperand& operand : std::span(operands, instruction.operand_count_visible)) {
                if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.base != ZYDIS_REGISTER_RIP)
                    continue;
                const std::uint64_t target = address + instruction.length + std::uint64_t(operand.mem.disp.value);
                if (image.isData(target))
                    result.data.push_back(target);
                else if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA && image.isCode(target))
                    result.code.push_back(target);
            }
        }
    }
    return references.emplace(function, std::move(result)).first->second;
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
