#include "game/native/coordinate_pool_internal/FindDec.h"
#include "game/native/CoordinatePoolRemotePlan.h"
#include "game/native/CoordinatePoolPolicy.h"
#include "game/native/coordinate_pool_internal/RingIndexCandidatePolicy.h"
#include "vendor/json.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::size_t gFreedInstructions = 0;
std::size_t gFreedDetails = 0;
constexpr std::uint32_t kSyntheticEntryStride = 80;

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned int index = 0; index < 4U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned int index = 0; index < 8U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void AppendVariable(std::vector<std::uint8_t>& bytes,
                    const std::string& name,
                    std::uint32_t type = 2U) {
    AppendU32(bytes, type);
    AppendU64(bytes, name.size());
    bytes.insert(bytes.end(), name.begin(), name.end());
}

std::string EncodeBase64(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes[offset]) << 16U |
            (offset + 1U < bytes.size()
                ? static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U
                : 0U) |
            (offset + 2U < bytes.size()
                ? static_cast<std::uint32_t>(bytes[offset + 2U])
                : 0U);
        encoded.push_back(kAlphabet[(value >> 18U) & 0x3FU]);
        encoded.push_back(kAlphabet[(value >> 12U) & 0x3FU]);
        encoded.push_back(offset + 1U < bytes.size()
            ? kAlphabet[(value >> 6U) & 0x3FU]
            : '=');
        encoded.push_back(offset + 2U < bytes.size()
            ? kAlphabet[value & 0x3FU]
            : '=');
    }
    return encoded;
}

std::string BuildRemotePlanPayload(std::uint64_t base) {
    using namespace lengjing::game::native::coordinate_pool_internal;
    std::vector<std::uint8_t> expression;
    AppendU32(expression, EXPR_BINARY);
    AppendU32(expression, OP_ADD);
    AppendVariable(expression, "ring", EXPR_MEMORY);
    AppendU32(expression, EXPR_BINARY);
    AppendU32(expression, OP_ADD);
    AppendVariable(expression, "memory", EXPR_MEMORY);
    AppendVariable(expression, "captured");

    nlohmann::json data = {
        {"A", 8},
        {"B", 16},
        {"C", 52},
        {"D", base},
        {"E", base + 4U},
        {"F", ARM64_REG_X2},
        {"G", base},
        {"H", base + 8U},
        {"I", ARM64_REG_X3},
        {"J", nlohmann::json::array({
            {{"A", "memory"}, {"B", 8},
             {"C", nlohmann::json::array()}, {"D", 32}},
        })},
        {"K", nlohmann::json::array({
            {{"A", "captured"}, {"B", base + 8U},
             {"C", ARM64_REG_X4}, {"D", 0}},
        })},
        {"L", EncodeBase64(expression)},
        {"M", "ring"},
        {"N", base + 12U},
        {"P", nlohmann::json::array({
            nlohmann::json::array({base + 16U, UINT32_C(0xD503201F)}),
        })},
    };
    return nlohmann::json{{"code", 0}, {"data", std::move(data)}}.dump();
}

std::string ReadFile(const char* path) {
    if (path == nullptr || *path == '\0') return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::uint64_t ReadUnsignedEnvironment(const char* name,
                                      std::uint64_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    std::size_t consumed = 0;
    const std::uint64_t parsed = std::stoull(value, &consumed, 10);
    if (consumed != std::strlen(value)) {
        throw std::invalid_argument("remote plan base is not decimal");
    }
    return parsed;
}

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
    const std::uint8_t* code,
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
        if (code != nullptr) {
            std::memcpy(result[index].bytes, code + index * 4, 4);
        }
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
    REQUIRE(lengjing::game::native::CoordinatePoolMaximumAnalysisPasses(
                false) == 8);
    REQUIRE(lengjing::game::native::CoordinatePoolMaximumAnalysisPasses(
                true) == 16);
    REQUIRE(!lengjing::game::native::
        ShouldExpandCoordinatePoolDecodeMethodScan(true, true, true));
    REQUIRE(!lengjing::game::native::
        ShouldExpandCoordinatePoolDecodeMethodScan(false, false, true));
    REQUIRE(!lengjing::game::native::
        ShouldExpandCoordinatePoolDecodeMethodScan(true, false, false));
    REQUIRE(lengjing::game::native::
        ShouldExpandCoordinatePoolDecodeMethodScan(true, false, true));
    REQUIRE(lengjing::game::native::
        CoordinatePoolRequestedMethodInstructionLimit(
            kBase, kBase, false) ==
        lengjing::game::native::
            kCoordinatePoolEntryAnalysisInstructionLimit);
    REQUIRE(lengjing::game::native::
        CoordinatePoolRequestedMethodInstructionLimit(
            kBase + 4U, kBase, true) ==
        lengjing::game::native::
            kCoordinatePoolEntryAnalysisInstructionLimit);
    REQUIRE(lengjing::game::native::
        CoordinatePoolRequestedMethodInstructionLimit(
            kBase + 4U, kBase, false) ==
        lengjing::game::native::
            kCoordinatePoolDecodeAnalysisInstructionLimit);
    REQUIRE(lengjing::game::native::
        NextCoordinatePoolDecodeAnalysisInstructionLimit(0) == 500);
    REQUIRE(lengjing::game::native::
        NextCoordinatePoolDecodeAnalysisInstructionLimit(500) == 1000);
    REQUIRE(lengjing::game::native::
        NextCoordinatePoolDecodeAnalysisInstructionLimit(1000) == 2000);
    REQUIRE(lengjing::game::native::
        NextCoordinatePoolDecodeAnalysisInstructionLimit(2000) == 4000);
    REQUIRE(lengjing::game::native::
        NextCoordinatePoolDecodeAnalysisInstructionLimit(4000) == 5000);
    REQUIRE(lengjing::game::native::
        NextCoordinatePoolDecodeAnalysisInstructionLimit(5000) == 5000);
    REQUIRE(lengjing::game::native::
        CoordinatePoolRequestedMethodInstructionLimit(
            kBase + 4U, kBase, false, 2000) == 2000);

    coord_dec::FindDec decodeLimitFinder;
    REQUIRE(decodeLimitFinder.decode_method_instruction_limit() == 500);
    decodeLimitFinder.set_decode_method_instruction_limit(2000);
    REQUIRE(decodeLimitFinder.decode_method_instruction_limit() == 2000);
    decodeLimitFinder.set_decode_method_instruction_limit(0);
    REQUIRE(decodeLimitFinder.decode_method_instruction_limit() == 500);
    REQUIRE(decodeLimitFinder.expected_entry_stride() == 0);
    REQUIRE(!decodeLimitFinder.set_expected_entry_stride(10));
    REQUIRE(decodeLimitFinder.expected_entry_stride() == 0);
    REQUIRE(decodeLimitFinder.set_expected_entry_stride(
        kSyntheticEntryStride));
    REQUIRE(decodeLimitFinder.expected_entry_stride() ==
        kSyntheticEntryStride);

    coord_dec::FindDec missingStrideFinder;
    REQUIRE(missingStrideFinder.find_dec(kBase) == -1);
    REQUIRE(missingStrideFinder.failure_stage() ==
        coord_dec::FindDecFailureStage::RingOffset);
    REQUIRE(missingStrideFinder.failure_detail() ==
        coord_dec::FindDecFailureDetail::EntryStrideMissing);

    std::array<std::uint32_t, 4> branchWrappedMethod{{
        UINT32_C(0x14000002),
        UINT32_C(0xD65F03C0),
        UINT32_C(0x14000001),
        UINT32_C(0xFC190FE8),
    }};
    coord_dec::FindDec branchWrappedFinder;
    REQUIRE(branchWrappedFinder.set(
        kBase,
        branchWrappedMethod.data(),
        static_cast<std::uint32_t>(sizeof(branchWrappedMethod))) == 0);
    REQUIRE(branchWrappedFinder.resolve_decode_method_entry(kBase) == kBase);
    REQUIRE(branchWrappedFinder.get_shellcode()
        ->requested_method_addresses().empty());
    branchWrappedFinder.set_resolve_decode_method_entry_branches(true);
    REQUIRE(branchWrappedFinder.resolve_decode_method_entry(kBase) ==
        kBase + 12U);
    const auto& branchRequests = branchWrappedFinder.get_shellcode()
        ->requested_method_addresses();
    REQUIRE(branchRequests.size() == 2);
    REQUIRE(branchRequests[0] == kBase);
    REQUIRE(branchRequests[1] == kBase + 8U);

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

    constexpr std::uint64_t kRemoteMappingBase = kBase - 0x1000U;
    constexpr std::uint64_t kRemoteEntry = kBase;
    constexpr std::size_t kRemotePlanCodeSize = 0x2000U;
    const std::string remotePayload = BuildRemotePlanPayload(kRemoteEntry);
    auto remotePlan = lengjing::game::native::ParseCoordinatePoolRemotePlan(
        remotePayload,
        kRemoteMappingBase,
        kRemotePlanCodeSize,
        kRemoteEntry);
    if (!remotePlan.Ok()) {
        throw std::runtime_error(remotePlan.detail);
    }
    REQUIRE(remotePlan.Ok());
    REQUIRE(remotePlan.plan.memoryParameters.size() == 1);
    REQUIRE(remotePlan.plan.variableParameters.size() == 1);
    REQUIRE(remotePlan.plan.patches.size() == 1);
    coord_dec::FindDec missingRemoteStrideFinder;
    REQUIRE(!missingRemoteStrideFinder.import_runtime_plan(remotePlan.plan));
    REQUIRE(missingRemoteStrideFinder.failure_detail() ==
        coord_dec::FindDecFailureDetail::EntryStrideMissing);

    nlohmann::json oversizedSignedPlan =
        nlohmann::json::parse(remotePayload);
    oversizedSignedPlan["data"]["B"] = UINT64_MAX;
    const auto rejectedSignedPlan =
        lengjing::game::native::ParseCoordinatePoolRemotePlan(
            oversizedSignedPlan.dump(),
            kRemoteMappingBase,
            kRemotePlanCodeSize,
            kRemoteEntry);
    REQUIRE(!rejectedSignedPlan.Ok());
    REQUIRE(rejectedSignedPlan.error ==
        lengjing::game::native::CoordinatePoolRemotePlanError::MissingField);

    std::array<std::uint8_t, kRemotePlanCodeSize> remoteCode{};
    coord_dec::FindDec remoteFinder;
    REQUIRE(remoteFinder.set_expected_entry_stride(kSyntheticEntryStride));
    REQUIRE(remoteFinder.set(
        kRemoteMappingBase,
        remoteCode.data(),
        static_cast<std::uint32_t>(remoteCode.size())) == 0);
    REQUIRE(remoteFinder.import_runtime_plan(std::move(remotePlan.plan)));
    method* remoteEntry = remoteFinder.get_shellcode()->get_method("entry");
    REQUIRE(remoteEntry != nullptr);
    REQUIRE(remoteEntry->get_point("v87_end")->address == kRemoteEntry + 4U);
    REQUIRE(remoteEntry->get_point("hash_end")->address == kRemoteEntry + 8U);
    REQUIRE(remoteEntry->get_point("all_params_exec_end")->address ==
        kRemoteEntry + 12U);
    remoteFinder.mem_param_list[0].value = 10;
    remoteFinder.analyze.varParams[0].value = 20;
    remoteFinder.setup_param();
    REQUIRE(remoteFinder.decode_ring_slot(5) == 35);
    std::array<std::uint8_t, kRemotePlanCodeSize> remotePatched{};
    REQUIRE(remoteFinder.get_shellcode()->apply_patches(
        kRemoteMappingBase, remotePatched.data(), remotePatched.size()));
    std::memcpy(
        &patchedInstruction,
        remotePatched.data() +
            static_cast<std::size_t>(
                kRemoteEntry - kRemoteMappingBase + 16U),
        sizeof(patchedInstruction));
    REQUIRE(patchedInstruction == kPatchedInstruction);

    const char* fixturePath = std::getenv("LENGJING_REMOTE_PLAN_FIXTURE");
    const char* dumpPath = std::getenv("LENGJING_REMOTE_PLAN_DUMP");
    if (fixturePath != nullptr || dumpPath != nullptr) {
        const std::string fixture = ReadFile(fixturePath);
        const std::string dump = ReadFile(dumpPath);
        REQUIRE(!fixture.empty());
        REQUIRE(!dump.empty());
        const nlohmann::json fixtureJson = nlohmann::json::parse(fixture);
        const std::uint64_t fixtureEntry =
            fixtureJson.at("data").at("D").get<std::uint64_t>();
        const std::uint64_t fixtureBase = ReadUnsignedEnvironment(
            "LENGJING_REMOTE_PLAN_BASE", fixtureEntry);
        auto fixturePlan =
            lengjing::game::native::ParseCoordinatePoolRemotePlan(
                fixture,
                fixtureBase,
                dump.size(),
                fixtureEntry);
        if (!fixturePlan.Ok()) {
            throw std::runtime_error(fixturePlan.detail);
        }
        REQUIRE(fixturePlan.Ok());
        REQUIRE(fixturePlan.plan.memoryParameters.size() == 3);
        REQUIRE(fixturePlan.plan.variableParameters.size() == 3);
        REQUIRE(fixturePlan.plan.patches.size() == 18);
        std::unordered_map<std::string, std::uint64_t> values;
        for (const auto& parameter : fixturePlan.plan.memoryParameters) {
            values[parameter.name] = parameter.value;
        }
        for (const auto& parameter : fixturePlan.plan.variableParameters) {
            values[parameter.name] = parameter.value;
        }
        values[fixturePlan.plan.ringIndexParameter] = 0;
        REQUIRE(fixturePlan.plan.indexExpression->eval(values) < 10U);
    }

    return 0;
}
