#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace lengjing::game::native {

class MemoryTransport final {
public:
    MemoryTransport();
    ~MemoryTransport();

    MemoryTransport(const MemoryTransport&) = delete;
    MemoryTransport& operator=(const MemoryTransport&) = delete;

    bool Open(int modeIndex,
              pid_t processId,
              std::string_view processName,
              std::string& error);
    void Close() noexcept;

    bool Read(std::uintptr_t address, void* destination, std::size_t size);
    std::uintptr_t ModuleBase(std::string_view moduleName);
    bool IsOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

pid_t FindProcessId(std::string_view processName);
bool IsProcessAlive(pid_t processId);
std::uintptr_t FindMappedModuleBase(pid_t processId, std::string_view moduleName);

}  // namespace lengjing::game::native
