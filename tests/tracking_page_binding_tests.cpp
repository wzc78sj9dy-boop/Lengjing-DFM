#include "game/native/TrackingPageBinding.h"

#include "test_support.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using lengjing::game::native::TrackingPageAccess;
using lengjing::game::native::TrackingPageBinding;
using lengjing::game::native::TrackingPageBindingBackend;
using lengjing::game::native::TrackingPageBindingOps;
using lengjing::game::native::TrackingPageBindingTicket;
namespace abi = lengjing::game::native::tracking_page_binding_abi;

constexpr std::uintptr_t kPatchAddress = 0x18000134;
constexpr std::uintptr_t kPatchPage = 0x18000000;
constexpr std::uintptr_t kSecondPatchAddress = 0x18002134;
constexpr std::uintptr_t kClonePage = 0x28000000;

struct Fixture final {
    std::array<std::uint8_t, abi::kPageSize> patch{};
    std::array<std::uint8_t, abi::kPageSize> secondPatch{};
    std::array<std::uint8_t, abi::kPageSize> clone{};
    std::vector<unsigned long> commands;
    std::vector<std::uint64_t> states;
    std::uintptr_t controlledPage = 0;
    std::uintptr_t boundPatch = 0;
    std::uintptr_t boundClone = 0;
    std::uintptr_t rewrittenPage = 0;
    int rewriteCount = 0;
    bool privateCommands = true;
    bool rewriteSuccess = true;

    Fixture() {
        patch.fill(0x41);
        secondPatch.fill(0x42);
    }
};

bool ReadPage(void* context,
              std::uintptr_t address,
              void* destination,
              std::size_t size) {
    auto& fixture = *static_cast<Fixture*>(context);
    if (destination == nullptr || size != abi::kPageSize) return false;
    const std::array<std::uint8_t, abi::kPageSize>* source = nullptr;
    if (address == kPatchPage) {
        source = &fixture.patch;
    } else if (address == kSecondPatchAddress -
                   (kSecondPatchAddress & (abi::kPageSize - 1U))) {
        source = &fixture.secondPatch;
    } else if (address == kClonePage) {
        source = &fixture.clone;
    }
    if (source == nullptr) return false;
    std::memcpy(destination, source->data(), source->size());
    return true;
}

bool WritePage(void* context,
               std::uintptr_t address,
               const void* source,
               std::size_t size) {
    auto& fixture = *static_cast<Fixture*>(context);
    if (address != kClonePage || source == nullptr ||
        size != fixture.clone.size()) {
        return false;
    }
    std::memcpy(fixture.clone.data(), source, fixture.clone.size());
    return true;
}

long Invoke(void* context,
            unsigned long command,
            void* request,
            std::size_t size,
            int& errorNumber) {
    auto& fixture = *static_cast<Fixture*>(context);
    fixture.commands.push_back(command);
    errorNumber = 0;
    if (!fixture.privateCommands) {
        errorNumber = EINVAL;
        return -1;
    }
    if (command == abi::kAllocateCommand) {
        REQUIRE(size == sizeof(abi::ClonePageRequest));
        auto& clone = *static_cast<abi::ClonePageRequest*>(request);
        REQUIRE(clone.processId == 321);
        REQUIRE(clone.size == abi::kPageSize);
        REQUIRE(clone.protection == 3);
        REQUIRE(clone.flags == 0x22);
        REQUIRE(clone.descriptor == -1);
        clone.mappedAddress = kClonePage;
        return 0;
    }
    if (command == abi::kPageControlCommand) {
        REQUIRE(size == sizeof(abi::PageControlRequest));
        const auto& control =
            *static_cast<abi::PageControlRequest*>(request);
        REQUIRE(control.processId == 321);
        REQUIRE(control.size == abi::kPageSize);
        REQUIRE(control.operation == 0x20000);
        fixture.controlledPage = control.pageAddress;
        fixture.states.push_back(control.state);
        return 0;
    }
    if (command == abi::kBindPageCommand) {
        REQUIRE(size == sizeof(abi::BindPageRequest));
        const auto& bind = *static_cast<abi::BindPageRequest*>(request);
        fixture.boundPatch = static_cast<std::uintptr_t>(
            bind.patchAddress);
        fixture.boundClone = static_cast<std::uintptr_t>(
            bind.clonedPage);
        return 0;
    }
    return -1;
}

bool Rewrite(void* context,
             pid_t processId,
             std::uintptr_t pageAddress,
             const void* source,
             std::size_t size) {
    auto& fixture = *static_cast<Fixture*>(context);
    REQUIRE(processId == 321);
    REQUIRE(source != nullptr);
    REQUIRE(size == abi::kPageSize);
    fixture.rewrittenPage = pageAddress;
    ++fixture.rewriteCount;
    return fixture.rewriteSuccess;
}

TrackingPageAccess Access(Fixture& fixture) {
    return {&fixture, &ReadPage, &WritePage};
}

TrackingPageBindingOps Operations(Fixture& fixture) {
    return {&fixture, &Invoke, &Rewrite};
}

void TestPrivateCommandSequence() {
    Fixture fixture;
    TrackingPageBinding binding(Operations(fixture));
    TrackingPageBindingTicket ticket{};
    REQUIRE(binding.PreparePatch(
        321, kPatchAddress, Access(fixture), ticket));
    REQUIRE(binding.Backend() ==
        TrackingPageBindingBackend::PrivateCommands);
    REQUIRE(ticket.pageAddress == kPatchPage);
    REQUIRE(ticket.clonedPage == kClonePage);
    REQUIRE(fixture.clone == fixture.patch);

    REQUIRE(binding.BeginPageWrite(321, kPatchAddress));
    REQUIRE(binding.EndPageWrite(321, kPatchAddress));
    REQUIRE(binding.CommitPatch(321, ticket));
    REQUIRE(ticket.committed);
    REQUIRE(binding.HasCommittedPrivateBinding());
    REQUIRE(fixture.commands.size() == 4);
    REQUIRE(fixture.commands[0] == abi::kAllocateCommand);
    REQUIRE(fixture.commands[1] == abi::kPageControlCommand);
    REQUIRE(fixture.commands[2] == abi::kPageControlCommand);
    REQUIRE(fixture.commands[3] == abi::kBindPageCommand);
    REQUIRE(fixture.controlledPage == kPatchPage);
    REQUIRE(fixture.states.size() == 2);
    REQUIRE(fixture.states[0] == 0);
    REQUIRE(fixture.states[1] == 1);
    REQUIRE(fixture.boundPatch == kPatchAddress);
    REQUIRE(fixture.boundClone == kClonePage);
}

void TestCopyOnWriteFallback() {
    Fixture fixture;
    fixture.privateCommands = false;
    TrackingPageBinding binding(Operations(fixture));
    TrackingPageBindingTicket first{};
    REQUIRE(binding.PreparePatch(
        321, kPatchAddress, Access(fixture), first));
    REQUIRE(binding.Backend() ==
        TrackingPageBindingBackend::CopyOnWrite);
    REQUIRE(first.clonedPage == 0);
    REQUIRE(fixture.rewriteCount == 1);
    REQUIRE(fixture.rewrittenPage == kPatchPage);

    TrackingPageBindingTicket second{};
    REQUIRE(binding.PreparePatch(
        321, kSecondPatchAddress, Access(fixture), second));
    REQUIRE(fixture.commands.size() == 1);
    REQUIRE(fixture.rewriteCount == 2);
    REQUIRE(binding.PreparePage(
        321, kPatchAddress, Access(fixture)));
    REQUIRE(fixture.rewriteCount == 3);
    REQUIRE(binding.BeginPageWrite(321, kPatchAddress));
    REQUIRE(binding.EndPageWrite(321, kPatchAddress));
    REQUIRE(binding.CommitPatch(321, first));
    REQUIRE(!binding.HasCommittedPrivateBinding());
}

void TestRewriteFailureStopsPreparation() {
    Fixture fixture;
    fixture.privateCommands = false;
    fixture.rewriteSuccess = false;
    TrackingPageBinding binding(Operations(fixture));
    TrackingPageBindingTicket ticket{};
    REQUIRE(!binding.PreparePatch(
        321, kPatchAddress, Access(fixture), ticket));
    REQUIRE(binding.Backend() ==
        TrackingPageBindingBackend::CopyOnWrite);
    REQUIRE(!ticket.committed);
}

}  // namespace

void RunTrackingPageBindingTests() {
    TestPrivateCommandSequence();
    TestCopyOnWriteFallback();
    TestRewriteFailureStopsPreparation();
}
