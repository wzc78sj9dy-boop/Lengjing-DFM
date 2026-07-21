#pragma once

#include <cstddef>
#include <cstdint>

#include <sys/types.h>

namespace lengjing::game::native {

namespace tracking_page_binding_abi {

inline constexpr std::size_t kPageSize = 0x1000;
inline constexpr unsigned long kAllocateCommand = 0x02220001UL;
inline constexpr unsigned long kPageControlCommand = 0x02220004UL;
inline constexpr unsigned long kBindPageCommand = 0x02220005UL;

struct ClonePageRequest final {
    std::int64_t processId;
    std::uint64_t mappedAddress;
    std::uint64_t size;
    std::uint64_t protection;
    std::uint64_t flags;
    std::int64_t descriptor;
    std::uint64_t offset;
};

struct PageControlRequest final {
    std::int64_t processId;
    std::uint64_t pageAddress;
    std::uint64_t size;
    std::uint64_t operation;
    std::uint64_t state;
    std::uint64_t reserved0;
    std::uint64_t reserved1;
};

struct BindPageRequest final {
    std::int64_t processId;
    std::uint64_t patchAddress;
    std::uint64_t clonedPage;
};

static_assert(sizeof(ClonePageRequest) == 0x38);
static_assert(sizeof(PageControlRequest) == 0x38);
static_assert(sizeof(BindPageRequest) == 0x18);

}  // namespace tracking_page_binding_abi

enum class TrackingPageBindingBackend : std::uint8_t {
    Unknown,
    PrivateCommands,
    CopyOnWrite,
};

struct TrackingPageAccess final {
    void* context = nullptr;
    bool (*read)(void* context,
                 std::uintptr_t address,
                 void* destination,
                 std::size_t size) = nullptr;
    bool (*write)(void* context,
                  std::uintptr_t address,
                  const void* source,
                  std::size_t size) = nullptr;
};

struct TrackingPageBindingOps final {
    void* context = nullptr;
    long (*invoke)(void* context,
                   unsigned long command,
                   void* request,
                   std::size_t size,
                   int& errorNumber) = nullptr;
    bool (*rewrite)(void* context,
                    pid_t processId,
                    std::uintptr_t pageAddress,
                    const void* source,
                    std::size_t size) = nullptr;
};

struct TrackingPageBindingTicket final {
    TrackingPageBindingBackend backend =
        TrackingPageBindingBackend::Unknown;
    std::uintptr_t patchAddress = 0;
    std::uintptr_t pageAddress = 0;
    std::uintptr_t clonedPage = 0;
    bool committed = false;
};

class TrackingPageBinding final {
public:
    explicit TrackingPageBinding(
        TrackingPageBindingOps operations = {}) noexcept;

    bool PreparePatch(pid_t processId,
                      std::uintptr_t patchAddress,
                      const TrackingPageAccess& access,
                      TrackingPageBindingTicket& ticket) noexcept;
    bool PreparePage(pid_t processId,
                     std::uintptr_t address,
                     const TrackingPageAccess& access) noexcept;
    bool BeginPageWrite(pid_t processId,
                        std::uintptr_t address) noexcept;
    bool EndPageWrite(pid_t processId,
                      std::uintptr_t address) noexcept;
    bool CommitPatch(pid_t processId,
                     TrackingPageBindingTicket& ticket) noexcept;

    TrackingPageBindingBackend Backend() const noexcept;
    bool HasCommittedPrivateBinding() const noexcept;
    void Reset() noexcept;

private:
    bool PreparePrivatePatch(
        pid_t processId,
        std::uintptr_t patchAddress,
        std::uintptr_t pageAddress,
        const TrackingPageAccess& access,
        TrackingPageBindingTicket& ticket) noexcept;
    bool RewriteAndVerify(pid_t processId,
                          std::uintptr_t pageAddress,
                          const TrackingPageAccess& access) noexcept;
    bool ControlPage(pid_t processId,
                     std::uintptr_t address,
                     std::uint64_t state) noexcept;
    long Invoke(unsigned long command,
                void* request,
                std::size_t size,
                int& errorNumber) noexcept;
    bool Rewrite(pid_t processId,
                 std::uintptr_t pageAddress,
                 const void* source,
                 std::size_t size) noexcept;

    TrackingPageBindingOps operations_{};
    TrackingPageBindingBackend backend_ =
        TrackingPageBindingBackend::Unknown;
    bool committedPrivateBinding_ = false;
};

}  // namespace lengjing::game::native
