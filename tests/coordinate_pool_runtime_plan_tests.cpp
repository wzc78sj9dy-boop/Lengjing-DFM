#include "game/native/coordinate_pool_internal/FindDec.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

std::size_t gFreedInstructions = 0;
std::size_t gFreedDetails = 0;

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error(                                         \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                ": requirement failed: " #condition);                       \
        }                                                                     \
    } while (false)

std::pair<cs_insn, cs_detail> MakeAdd(
    arm64_reg destination,
    arm64_reg lhs,
    arm64_reg rhs,
    arm64_shifter shift,
    unsigned int amount,
    arm64_extender extender = ARM64_EXT_INVALID) {
    cs_insn instruction{};
    cs_detail detail{};
    instruction.id = ARM64_INS_ADD;
    instruction.address = UINT64_C(0x1000);
    instruction.detail = &detail;
    detail.arm64.op_count = 3;
    detail.arm64.operands[0].type = ARM64_OP_REG;
    detail.arm64.operands[0].reg = destination;
    detail.arm64.operands[0].access = CS_AC_WRITE;
    detail.arm64.operands[1].type = ARM64_OP_REG;
    detail.arm64.operands[1].reg = lhs;
    detail.arm64.operands[1].access = CS_AC_READ;
    detail.arm64.operands[2].type = ARM64_OP_REG;
    detail.arm64.operands[2].reg = rhs;
    detail.arm64.operands[2].access = CS_AC_READ;
    detail.arm64.operands[2].shift.type = shift;
    detail.arm64.operands[2].shift.value = amount;
    detail.arm64.operands[2].ext = extender;
    return {instruction, detail};
}

}  // namespace

extern "C" cs_err cs_open(cs_arch, cs_mode, csh* handle) {
    if (handle == nullptr) return CS_ERR_CSH;
    *handle = 1;
    return CS_ERR_OK;
}

extern "C" cs_err cs_close(csh* handle) {
    if (handle == nullptr) return CS_ERR_CSH;
    *handle = 0;
    return CS_ERR_OK;
}

extern "C" cs_err cs_option(csh, cs_opt_type, std::size_t) {
    return CS_ERR_OK;
}

extern "C" std::size_t cs_disasm(
    csh,
    const std::uint8_t*,
    std::size_t codeSize,
    std::uint64_t address,
    std::size_t requestedCount,
    cs_insn** instructions) {
    if (instructions == nullptr || codeSize < 8) return 0;
    std::size_t count = codeSize / 4;
    if (requestedCount != 0 && requestedCount < count) {
        count = requestedCount;
    }
    auto* result = static_cast<cs_insn*>(
        std::calloc(count, sizeof(cs_insn)));
    if (result == nullptr) return 0;
    for (std::size_t index = 0; index < count; ++index) {
        result[index].address = address + index * 4;
        result[index].size = 4;
        result[index].id = index + 1 == count
            ? ARM64_INS_RET
            : ARM64_INS_NOP;
        result[index].detail = static_cast<cs_detail*>(
            std::calloc(1, sizeof(cs_detail)));
        if (result[index].detail == nullptr) {
            cs_free(result, count);
            return 0;
        }
    }
    *instructions = result;
    return count;
}

extern "C" void cs_free(cs_insn* instructions, std::size_t count) {
    if (instructions == nullptr) return;
    for (std::size_t index = 0; index < count; ++index) {
        if (instructions[index].detail != nullptr) {
            std::free(instructions[index].detail);
            ++gFreedDetails;
        }
    }
    std::free(instructions);
    gFreedInstructions += count;
}

int main() {
    using namespace lengjing::game::native::coordinate_pool_internal;

    constexpr std::uint64_t kBase = UINT64_C(0x100000);
    std::array<std::uint8_t, 8> code{};

    Analyze legacyAdd;
    legacyAdd.setVal(ARM64_REG_X1, "legacyLhs");
    legacyAdd.setVal(ARM64_REG_X2, "legacyRhs");
    auto normalAdd = MakeAdd(
        ARM64_REG_X0, ARM64_REG_X1, ARM64_REG_X2,
        ARM64_SFT_LSL, 2);
    normalAdd.first.detail = &normalAdd.second;
    REQUIRE(legacyAdd.parse(&normalAdd.first) == 0);
    std::unordered_map<std::string, std::uint64_t> legacyParams{
        {"legacyLhs", 5},
        {"legacyRhs", 3},
    };
    REQUIRE(legacyAdd.execute(ARM64_REG_X0, legacyParams) == 17);

    Analyze shiftedAdd;
    shiftedAdd.setVal(ARM64_REG_X1, "shiftedLhs");
    shiftedAdd.setVal(ARM64_REG_X2, "shiftedRhs");
    auto asrAdd = MakeAdd(
        ARM64_REG_X0, ARM64_REG_X1, ARM64_REG_X2,
        ARM64_SFT_ASR, 4);
    asrAdd.first.detail = &asrAdd.second;
    REQUIRE(shiftedAdd.parse(&asrAdd.first) != 0);
    REQUIRE(shiftedAdd.parse_add_compat(&asrAdd.first) == 0);
    std::unordered_map<std::string, std::uint64_t> shiftedParams{
        {"shiftedLhs", 5},
        {"shiftedRhs", UINT64_C(0xFFFFFFFFFFFFFFF0)},
    };
    REQUIRE(shiftedAdd.execute(ARM64_REG_X0, shiftedParams) == 4);

    Analyze extendedAdd;
    extendedAdd.setVal(ARM64_REG_X1, "extendedBase");
    extendedAdd.setVal(ARM64_REG_W2, "extendedOffset");
    auto sxtwAdd = MakeAdd(
        ARM64_REG_X0, ARM64_REG_X1, ARM64_REG_W2,
        ARM64_SFT_LSL, 2, ARM64_EXT_SXTW);
    sxtwAdd.first.detail = &sxtwAdd.second;
    REQUIRE(extendedAdd.parse_add_compat(&sxtwAdd.first) == 0);
    std::unordered_map<std::string, std::uint64_t> extendedParams{
        {"extendedBase", 10},
        {"extendedOffset", UINT64_C(0xFFFFFFFF)},
    };
    REQUIRE(extendedAdd.execute(ARM64_REG_X0, extendedParams) == 6);

    Analyze specialAdd;
    specialAdd.setVal(ARM64_REG_X29, "frame");
    auto frameLinkAdd = MakeAdd(
        ARM64_REG_X0, ARM64_REG_X29, ARM64_REG_X30,
        ARM64_SFT_INVALID, 0);
    frameLinkAdd.first.detail = &frameLinkAdd.second;
    REQUIRE(specialAdd.parse(&frameLinkAdd.first) != 0);
    REQUIRE(specialAdd.parse_add_compat(&frameLinkAdd.first) == 0);
    REQUIRE(specialAdd.varParams.size() == 1);
    REQUIRE(specialAdd.varParams[0].reg == ARM64_REG_X30);
    std::unordered_map<std::string, std::uint64_t> specialParams{
        {"frame", 7},
        {specialAdd.varParams[0].name, 8},
    };
    REQUIRE(specialAdd.execute(ARM64_REG_X0, specialParams) == 15);

    Analyze stackAdd;
    auto stackImmediate = MakeAdd(
        ARM64_REG_X0, ARM64_REG_SP, ARM64_REG_XZR,
        ARM64_SFT_INVALID, 0);
    stackImmediate.first.detail = &stackImmediate.second;
    stackImmediate.second.arm64.operands[2].type = ARM64_OP_IMM;
    stackImmediate.second.arm64.operands[2].imm = 32;
    REQUIRE(stackAdd.parse(&stackImmediate.first) != 0);
    REQUIRE(stackAdd.parse_add_compat(&stackImmediate.first) == 0);
    REQUIRE(stackAdd.varParams.size() == 1);
    REQUIRE(stackAdd.varParams[0].reg == ARM64_REG_SP);
    std::unordered_map<std::string, std::uint64_t> stackParams{
        {stackAdd.varParams[0].name, UINT64_C(0x2000)},
    };
    REQUIRE(stackAdd.execute(ARM64_REG_X0, stackParams) ==
        UINT64_C(0x2020));

    Analyze vectorAdd;
    vectorAdd.setVal(ARM64_REG_X1, "scalarState");
    auto vectorInstruction = MakeAdd(
        ARM64_REG_V0, ARM64_REG_V1, ARM64_REG_V2,
        ARM64_SFT_INVALID, 0);
    vectorInstruction.first.detail = &vectorInstruction.second;
    REQUIRE(vectorAdd.parse(&vectorInstruction.first) != 0);
    REQUIRE(vectorAdd.parse_add_compat(&vectorInstruction.first) == 0);
    REQUIRE(vectorAdd.str(ARM64_REG_X1) == "scalarState");

    Analyze invalidAdd;
    invalidAdd.setVal(ARM64_REG_X1, "invalidLhs");
    invalidAdd.setVal(ARM64_REG_X2, "invalidRhs");
    auto invalidShift = MakeAdd(
        ARM64_REG_X0, ARM64_REG_X1, ARM64_REG_X2,
        ARM64_SFT_ROR, 1);
    invalidShift.first.detail = &invalidShift.second;
    REQUIRE(invalidAdd.parse_add_compat(&invalidShift.first) != 0);

    coord_dec::FindDec missingEntryFinder;
    REQUIRE(missingEntryFinder.set(
        kBase, code.data(), static_cast<std::uint32_t>(code.size())) == 0);
    REQUIRE(missingEntryFinder.find_dec(kBase + code.size()) != 0);
    REQUIRE(missingEntryFinder.failure_stage() ==
        coord_dec::FindDecFailureStage::EntryMethod);
    REQUIRE(missingEntryFinder.failure_detail() ==
        coord_dec::FindDecFailureDetail::None);

    coord_dec::FindDec missingMarkerFinder;
    REQUIRE(missingMarkerFinder.set(
        kBase, code.data(), static_cast<std::uint32_t>(code.size())) == 0);
    REQUIRE(missingMarkerFinder.find_dec(kBase) != 0);
    REQUIRE(missingMarkerFinder.failure_stage() ==
        coord_dec::FindDecFailureStage::V87Marker);
    REQUIRE(missingMarkerFinder.failure_detail() ==
        coord_dec::FindDecFailureDetail::None);
    REQUIRE(missingMarkerFinder.madd_count() == 0);
    REQUIRE(missingMarkerFinder.ring_madd_count() == 0);
    REQUIRE(missingMarkerFinder.candidate_count() == 0);
    REQUIRE(missingMarkerFinder.failure_instruction() == 0);

    coord_dec::FindDec finder;
    shellcode* binary = finder.get_shellcode();
    REQUIRE(binary->parse(kBase, code.data(), code.size()) == 0);
    REQUIRE(binary->data() != nullptr);

    lengjing::game::native::coordinate_pool_internal::finder methodEnd;
    methodEnd.is_ret();
    REQUIRE(binary->create_method("entry", kBase, methodEnd, 2) != nullptr);
    REQUIRE(binary->get_method("entry") != nullptr);
    REQUIRE(binary->requested_method_addresses().size() == 1);

    constexpr std::uint32_t kPatchedInstruction = UINT32_C(0xD503201F);
    std::uint32_t patchInstruction = kPatchedInstruction;
    binary->patch(0, &patchInstruction, 4);

    finder.mem_param_list.push_back(
        {"memory", 8, 0, 10, {}});
    finder.analyze.varParams.push_back(
        {"captured", kBase + 4, ARM64_REG_X2, 20});
    finder.analyze.setVal(ARM64_REG_X1, "analysis_only");
    finder.index_expr = std::make_shared<BinaryExpr>(
        OP_ADD,
        std::make_shared<VarExpr>("", 0),
        std::make_shared<BinaryExpr>(
            OP_ADD,
            std::make_shared<VarExpr>("memory", 0),
            std::make_shared<VarExpr>("captured", 0),
            0),
        0);
    finder.setup_param();
    REQUIRE(finder.decode_ring_slot(5) == 35);
    REQUIRE(finder.analyze.str(ARM64_REG_X1) == "analysis_only");

    finder.compact_runtime_plan();

    REQUIRE(gFreedInstructions == 2);
    REQUIRE(gFreedDetails == 2);
    REQUIRE(binary->data() == nullptr);
    REQUIRE(binary->get_method("entry") == nullptr);
    REQUIRE(binary->requested_method_addresses().empty());
    REQUIRE(binary->start_addr() == kBase);
    REQUIRE(binary->end_addr() == kBase + code.size());
    REQUIRE(binary->size() == code.size());
    REQUIRE(finder.analyze.str(ARM64_REG_X1) == "[null]");
    REQUIRE(finder.analyze.varParams.size() == 1);
    REQUIRE(finder.analyze.varParams[0].value == 20);
    REQUIRE(finder.mem_param_list.size() == 1);
    REQUIRE(finder.decode_ring_slot(6) == 36);

    finder.mem_param_list[0].value = 15;
    finder.analyze.varParams[0].value = 25;
    finder.setup_param();
    REQUIRE(finder.decode_ring_slot(6) == 46);

    std::array<std::uint8_t, 8> patchedPage{};
    REQUIRE(binary->apply_patches(
        kBase, patchedPage.data(), patchedPage.size()));
    std::uint32_t patchedInstruction = 0;
    std::memcpy(
        &patchedInstruction,
        patchedPage.data(),
        sizeof(patchedInstruction));
    REQUIRE(patchedInstruction == kPatchedInstruction);
    return 0;
}
