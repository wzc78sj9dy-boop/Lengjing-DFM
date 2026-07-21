#include "game/native/TrackingPageBinding.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#if defined(__ANDROID__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace lengjing::game::native {
namespace {

using tracking_page_binding_abi::BindPageRequest;
using tracking_page_binding_abi::ClonePageRequest;
using tracking_page_binding_abi::PageControlRequest;

constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x10000000000ULL;

std::uintptr_t AlignPage(std::uintptr_t address) noexcept {
    return address & ~static_cast<std::uintptr_t>(
        tracking_page_binding_abi::kPageSize - 1U);
}

bool IsRemotePageValid(std::uintptr_t address) noexcept {
    return address >= kMinimumRemoteAddress &&
        address < kMaximumRemoteAddress &&
        (address & (tracking_page_binding_abi::kPageSize - 1U)) == 0;
}

bool IsUnsupportedCommandError(int errorNumber) noexcept {
    return errorNumber == EINVAL || errorNumber == ENOSYS ||
        errorNumber == ENOTTY || errorNumber == EOPNOTSUPP;
}

long InvokeSystemCommand(unsigned long command,
                         void* request,
                         std::size_t size,
                         int& errorNumber) noexcept {
#if defined(__ANDROID__) || defined(__linux__)
    errno = 0;
    const long result = syscall(
        __NR_prctl,
        command,
        request,
        size,
        0UL,
        0UL);
    errorNumber = errno;
    return result;
#else
    (void)command;
    (void)request;
    (void)size;
    errorNumber = ENOSYS;
    return -1;
#endif
}

bool RewriteProcessPage(pid_t processId,
                        std::uintptr_t pageAddress,
                        const void* source,
                        std::size_t size) noexcept {
#if defined(__ANDROID__) || defined(__linux__)
    if (processId <= 0 || !IsRemotePageValid(pageAddress) ||
        source == nullptr ||
        size != tracking_page_binding_abi::kPageSize ||
        pageAddress > static_cast<std::uintptr_t>(
            std::numeric_limits<off_t>::max())) {
        return false;
    }

    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/mem", processId);
    const int descriptor = open(path, O_RDWR | O_CLOEXEC);
    if (descriptor < 0) return false;

    const auto* bytes = static_cast<const std::uint8_t*>(source);
    std::size_t completed = 0;
    bool success = true;
    while (completed < size) {
        const ssize_t result = pwrite(
            descriptor,
            bytes + completed,
            size - completed,
            static_cast<off_t>(pageAddress + completed));
        if (result > 0) {
            completed += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        success = false;
        break;
    }
    if (close(descriptor) != 0) success = false;
    return success && completed == size;
#else
    (void)processId;
    (void)pageAddress;
    (void)source;
    (void)size;
    return false;
#endif
}

bool AccessValid(const TrackingPageAccess& access) noexcept {
    return access.context != nullptr && access.read != nullptr &&
        access.write != nullptr;
}

}  // namespace

TrackingPageBinding::TrackingPageBinding(
    TrackingPageBindingOps operations) noexcept
    : operations_(operations) {}

bool TrackingPageBinding::PreparePatch(
    pid_t processId,
    std::uintptr_t patchAddress,
    const TrackingPageAccess& access,
    TrackingPageBindingTicket& ticket) noexcept {
    ticket = {};
    if (processId <= 0 || !AccessValid(access) ||
        patchAddress < kMinimumRemoteAddress ||
        patchAddress >= kMaximumRemoteAddress) {
        return false;
    }

    const std::uintptr_t pageAddress = AlignPage(patchAddress);
    if (!IsRemotePageValid(pageAddress)) return false;

    if (backend_ == TrackingPageBindingBackend::Unknown ||
        backend_ == TrackingPageBindingBackend::PrivateCommands) {
        if (PreparePrivatePatch(
                processId,
                patchAddress,
                pageAddress,
                access,
                ticket)) {
            backend_ = TrackingPageBindingBackend::PrivateCommands;
            return true;
        }
        if (backend_ == TrackingPageBindingBackend::PrivateCommands) {
            return false;
        }
        if (backend_ == TrackingPageBindingBackend::Unknown) return false;
    }

    if (backend_ != TrackingPageBindingBackend::CopyOnWrite ||
        !RewriteAndVerify(processId, pageAddress, access)) {
        return false;
    }
    ticket.backend = TrackingPageBindingBackend::CopyOnWrite;
    ticket.patchAddress = patchAddress;
    ticket.pageAddress = pageAddress;
    return true;
}

bool TrackingPageBinding::PreparePrivatePatch(
    pid_t processId,
    std::uintptr_t patchAddress,
    std::uintptr_t pageAddress,
    const TrackingPageAccess& access,
    TrackingPageBindingTicket& ticket) noexcept {
    std::array<std::uint8_t,
               tracking_page_binding_abi::kPageSize> original{};
    if (!access.read(
            access.context,
            pageAddress,
            original.data(),
            original.size())) {
        return false;
    }

    ClonePageRequest request{};
    request.processId = processId;
    request.size = tracking_page_binding_abi::kPageSize;
#if defined(PROT_READ) && defined(PROT_WRITE)
    request.protection = PROT_READ | PROT_WRITE;
#else
    request.protection = 3;
#endif
#if defined(MAP_PRIVATE) && defined(MAP_ANONYMOUS)
    request.flags = MAP_PRIVATE | MAP_ANONYMOUS;
#else
    request.flags = 0x22;
#endif
    request.descriptor = -1;

    int errorNumber = 0;
    const long result = Invoke(
        tracking_page_binding_abi::kAllocateCommand,
        &request,
        sizeof(request),
        errorNumber);
    if (result < 0 || request.mappedAddress == 0) {
        if (backend_ == TrackingPageBindingBackend::Unknown &&
            result < 0 && IsUnsupportedCommandError(errorNumber)) {
            backend_ = TrackingPageBindingBackend::CopyOnWrite;
        }
        return false;
    }

    const std::uintptr_t clonedPage = static_cast<std::uintptr_t>(
        request.mappedAddress);
    if (!IsRemotePageValid(clonedPage) ||
        !access.write(
            access.context,
            clonedPage,
            original.data(),
            original.size())) {
        return false;
    }

    std::array<std::uint8_t,
               tracking_page_binding_abi::kPageSize> observed{};
    if (!access.read(
            access.context,
            clonedPage,
            observed.data(),
            observed.size()) ||
        observed != original) {
        return false;
    }

    ticket.backend = TrackingPageBindingBackend::PrivateCommands;
    ticket.patchAddress = patchAddress;
    ticket.pageAddress = pageAddress;
    ticket.clonedPage = clonedPage;
    return true;
}

bool TrackingPageBinding::PreparePage(
    pid_t processId,
    std::uintptr_t address,
    const TrackingPageAccess& access) noexcept {
    if (processId <= 0 || !AccessValid(access) ||
        backend_ == TrackingPageBindingBackend::Unknown) {
        return false;
    }
    if (backend_ == TrackingPageBindingBackend::PrivateCommands) {
        return true;
    }
    return RewriteAndVerify(processId, AlignPage(address), access);
}

bool TrackingPageBinding::RewriteAndVerify(
    pid_t processId,
    std::uintptr_t pageAddress,
    const TrackingPageAccess& access) noexcept {
    if (!IsRemotePageValid(pageAddress)) return false;
    std::array<std::uint8_t,
               tracking_page_binding_abi::kPageSize> original{};
    if (!access.read(
            access.context,
            pageAddress,
            original.data(),
            original.size()) ||
        !Rewrite(
            processId,
            pageAddress,
            original.data(),
            original.size())) {
        return false;
    }

    std::array<std::uint8_t,
               tracking_page_binding_abi::kPageSize> observed{};
    return access.read(
               access.context,
               pageAddress,
               observed.data(),
               observed.size()) &&
        observed == original;
}

bool TrackingPageBinding::BeginPageWrite(
    pid_t processId,
    std::uintptr_t address) noexcept {
    return backend_ != TrackingPageBindingBackend::Unknown &&
        (backend_ == TrackingPageBindingBackend::CopyOnWrite ||
         ControlPage(processId, address, 0));
}

bool TrackingPageBinding::EndPageWrite(
    pid_t processId,
    std::uintptr_t address) noexcept {
    return backend_ != TrackingPageBindingBackend::Unknown &&
        (backend_ == TrackingPageBindingBackend::CopyOnWrite ||
         ControlPage(processId, address, 1));
}

bool TrackingPageBinding::ControlPage(
    pid_t processId,
    std::uintptr_t address,
    std::uint64_t state) noexcept {
    if (processId <= 0 || !IsRemotePageValid(AlignPage(address))) {
        return false;
    }
    PageControlRequest request{};
    request.processId = processId;
    request.pageAddress = AlignPage(address);
    request.size = tracking_page_binding_abi::kPageSize;
    request.operation = 0x20000;
    request.state = state;
    int errorNumber = 0;
    return Invoke(
               tracking_page_binding_abi::kPageControlCommand,
               &request,
               sizeof(request),
               errorNumber) >= 0;
}

bool TrackingPageBinding::CommitPatch(
    pid_t processId,
    TrackingPageBindingTicket& ticket) noexcept {
    if (ticket.committed || processId <= 0 ||
        ticket.backend != backend_ || ticket.patchAddress == 0) {
        return false;
    }
    if (backend_ == TrackingPageBindingBackend::CopyOnWrite) {
        ticket.committed = true;
        return true;
    }
    if (backend_ != TrackingPageBindingBackend::PrivateCommands ||
        !IsRemotePageValid(ticket.clonedPage)) {
        return false;
    }

    BindPageRequest request{
        processId,
        static_cast<std::uint64_t>(ticket.patchAddress),
        static_cast<std::uint64_t>(ticket.clonedPage),
    };
    int errorNumber = 0;
    if (Invoke(
            tracking_page_binding_abi::kBindPageCommand,
            &request,
            sizeof(request),
            errorNumber) < 0) {
        return false;
    }
    ticket.committed = true;
    committedPrivateBinding_ = true;
    return true;
}

long TrackingPageBinding::Invoke(unsigned long command,
                                 void* request,
                                 std::size_t size,
                                 int& errorNumber) noexcept {
    if (operations_.invoke != nullptr) {
        return operations_.invoke(
            operations_.context,
            command,
            request,
            size,
            errorNumber);
    }
    return InvokeSystemCommand(command, request, size, errorNumber);
}

bool TrackingPageBinding::Rewrite(pid_t processId,
                                  std::uintptr_t pageAddress,
                                  const void* source,
                                  std::size_t size) noexcept {
    if (operations_.rewrite != nullptr) {
        return operations_.rewrite(
            operations_.context,
            processId,
            pageAddress,
            source,
            size);
    }
    return RewriteProcessPage(processId, pageAddress, source, size);
}

TrackingPageBindingBackend TrackingPageBinding::Backend() const noexcept {
    return backend_;
}

bool TrackingPageBinding::HasCommittedPrivateBinding() const noexcept {
    return committedPrivateBinding_;
}

void TrackingPageBinding::Reset() noexcept {
    backend_ = TrackingPageBindingBackend::Unknown;
    committedPrivateBinding_ = false;
}

}  // namespace lengjing::game::native
