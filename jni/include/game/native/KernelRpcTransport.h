#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lengjing::game::native {

namespace kernel_rpc_abi {

constexpr long kSyscallNumber = 63;
constexpr std::uint64_t kSentinelDescriptor =
    UINT64_C(0x00000000FFFFFFFF);
constexpr std::uint64_t kProtocolMagic = UINT64_C(0xA56B848D6A19F707);
constexpr std::size_t kBatchLimit = 1024;
using ProcessHandle = std::uint32_t;
constexpr ProcessHandle kInvalidProcessHandle = 0;

enum class Operation : std::uint32_t {
    InstallApgaKey = 2134,
    ControlWord = 5238,
    FindModuleBase = 5369,
    WriteMemory = 8625,
    FindProcessId = 11602,
    ReadMemory = 13977,
    ReleaseProcessHandle = 25727,
    WriteMemoryBatch = 30851,
    ReadTpidrEl0 = 45206,
    ReadProcessPacKeys = 46630,
    ReadMemoryBatch = 64836,
};

struct Envelope {
    std::uint32_t operation;
    std::uint32_t reserved;
    void* payload;
};

struct SingleMemoryPayload {
    ProcessHandle processHandle;
    std::uint32_t reserved;
    std::uint64_t remoteAddress;
    std::uint64_t localAddress;
    std::uint64_t size;
};

struct BatchMemoryItem {
    std::uint64_t remoteAddress;
    std::uint64_t localAddress;
    std::uint64_t size;
};

struct BatchWritePayload {
    ProcessHandle processHandle;
    std::uint32_t count;
    BatchMemoryItem* items;
};

struct BatchReadPayload {
    ProcessHandle processHandle;
    std::uint32_t count;
    BatchMemoryItem* items;
    std::uint8_t* status;
};

struct ProcessLookupPayload {
    char processName[64];
    std::int32_t processId;
};

struct ModuleLookupPayload {
    ProcessHandle processHandle;
    char moduleName[64];
    std::uint32_t reserved;
    std::uint64_t moduleBase;
};

struct ProcessValuePayload {
    std::uint32_t processId;
    std::uint32_t reserved;
    std::uint64_t value;
};

struct PacKey128 {
    std::uint64_t low;
    std::uint64_t high;
};

struct ProcessPacKeysPayload {
    std::int32_t processId;
    std::uint32_t reserved;
    PacKey128 apia;
    PacKey128 apib;
    PacKey128 apda;
    PacKey128 apdb;
    PacKey128 apga;
};

static_assert(sizeof(void*) == 8, "kernel RPC requires a 64-bit process");
static_assert(sizeof(Envelope) == 16, "bad RPC envelope layout");
static_assert(offsetof(Envelope, payload) == 8, "bad RPC payload offset");
static_assert(sizeof(SingleMemoryPayload) == 32,
              "bad single-memory payload layout");
static_assert(sizeof(BatchMemoryItem) == 24, "bad batch item layout");
static_assert(sizeof(BatchWritePayload) == 16,
              "bad batch-write payload layout");
static_assert(sizeof(BatchReadPayload) == 24,
              "bad batch-read payload layout");
static_assert(sizeof(ProcessLookupPayload) == 68,
              "bad process lookup payload layout");
static_assert(sizeof(ModuleLookupPayload) == 80,
              "bad module lookup payload layout");
static_assert(offsetof(ModuleLookupPayload, moduleBase) == 72,
              "bad module base output offset");
static_assert(sizeof(ProcessValuePayload) == 16,
              "bad process value payload layout");
static_assert(sizeof(PacKey128) == 16, "bad PAC key layout");
static_assert(sizeof(ProcessPacKeysPayload) == 88,
              "bad process PAC payload layout");
static_assert(offsetof(ProcessPacKeysPayload, apia) == 8,
              "bad APIA offset");
static_assert(offsetof(ProcessPacKeysPayload, apib) == 24,
              "bad APIB offset");
static_assert(offsetof(ProcessPacKeysPayload, apda) == 40,
              "bad APDA offset");
static_assert(offsetof(ProcessPacKeysPayload, apdb) == 56,
              "bad APDB offset");
static_assert(offsetof(ProcessPacKeysPayload, apga) == 72,
              "bad APGA offset");

}  // namespace kernel_rpc_abi

struct MutableMemoryTransfer {
    std::uintptr_t remoteAddress = 0;
    void* localBuffer = nullptr;
    std::size_t size = 0;
};

struct ConstMemoryTransfer {
    std::uintptr_t remoteAddress = 0;
    const void* localBuffer = nullptr;
    std::size_t size = 0;
};

class KernelRpcTransport final {
public:
    using SyscallInvoker = long (*)(long number,
                                    std::uint64_t descriptor,
                                    void* envelope,
                                    std::uint64_t magic);

    explicit KernelRpcTransport(SyscallInvoker invoker = nullptr) noexcept;

    int ReadMemory(kernel_rpc_abi::ProcessHandle processHandle,
                   std::uintptr_t remoteAddress,
                   void* destination,
                   std::size_t size) const;
    int WriteMemory(kernel_rpc_abi::ProcessHandle processHandle,
                    std::uintptr_t remoteAddress,
                    const void* source,
                    std::size_t size) const;

    // Returns the number of successful items, or a negative errno-style value.
    int ReadMemoryBatch(kernel_rpc_abi::ProcessHandle processHandle,
                        const MutableMemoryTransfer* transfers,
                        std::size_t count,
                        std::uint8_t* itemStatus = nullptr) const;
    int WriteMemoryBatch(kernel_rpc_abi::ProcessHandle processHandle,
                         const ConstMemoryTransfer* transfers,
                         std::size_t count) const;

    int FindProcessId(std::string_view processName,
                      std::int32_t& processId) const;
    // The command-5238 attach payload is not yet resolved. Until it is,
    // callers cannot obtain a handle accepted by memory/module operations.
    int AttachProcess(
        std::int32_t processId,
        kernel_rpc_abi::ProcessHandle& processHandle) const;
    int FindModuleBase(kernel_rpc_abi::ProcessHandle processHandle,
                       std::string_view moduleName,
                       std::uint64_t& moduleBase) const;
    int ReadTpidrEl0(std::int32_t processId, std::uint64_t& value) const;
    int ReadProcessPacKeys(
        std::int32_t processId,
        kernel_rpc_abi::ProcessPacKeysPayload& keys) const;
    // The installed key belongs to the calling thread. Invoke this on the
    // same OS thread that executes PACGA-dependent code.
    int InstallApgaKey(const kernel_rpc_abi::PacKey128& key) const;
    int ReleaseProcessHandle(
        kernel_rpc_abi::ProcessHandle processHandle) const;

    // Operation 5238 has a confirmed 64-bit payload but unresolved semantics.
    int InvokeControlWord(std::uint64_t& value) const;

private:
    static long InvokeSystemCall(long number,
                                 std::uint64_t descriptor,
                                 void* envelope,
                                 std::uint64_t magic);
    int Invoke(kernel_rpc_abi::Operation operation, void* payload) const;

    SyscallInvoker invoker_;
};

}  // namespace lengjing::game::native
