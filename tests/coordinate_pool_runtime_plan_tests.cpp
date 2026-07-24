#include "game/native/coordinate_pool_internal/FindDec.h"
#include "game/native/coordinate_pool_internal/RingIndexCandidatePolicy.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

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

    const auto makeModuloEight = [](std::uint64_t increment) {
        std::shared_ptr<Expr> value =
            std::make_shared<VarExpr>("index", 0);
        if (increment != 0) {
            value = std::make_shared<BinaryExpr>(
                OP_ADD,
                std::move(value),
                std::make_shared<ConstExpr>(increment, 0),
                0);
        }
        return std::make_shared<BinaryExpr>(
            OP_AND,
            std::move(value),
            std::make_shared<ConstExpr>(7, 0),
            0);
    };
    const std::set<std::string> indexDependencies{"index"};
    const auto currentIndex = makeModuloEight(0);
    const auto nextIndex = makeModuloEight(1);
    const auto skippedIndex = makeModuloEight(2);
    const auto forwardRelation =
        coord_dec::DetectRingIndexSuccessorRelation(
            currentIndex, nextIndex, indexDependencies);
    REQUIRE(forwardRelation.currentCandidate == 0);
    REQUIRE(forwardRelation.modulus == 8);
    const auto reverseRelation =
        coord_dec::DetectRingIndexSuccessorRelation(
            nextIndex, currentIndex, indexDependencies);
    REQUIRE(reverseRelation.currentCandidate == 1);
    REQUIRE(reverseRelation.modulus == 8);
    const auto unrelatedRelation =
        coord_dec::DetectRingIndexSuccessorRelation(
            currentIndex, skippedIndex, indexDependencies);
    REQUIRE(unrelatedRelation.currentCandidate == -1);
    REQUIRE(unrelatedRelation.modulus == 0);
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
