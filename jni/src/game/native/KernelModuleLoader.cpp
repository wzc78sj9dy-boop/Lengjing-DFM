#include "game/native/KernelModuleLoader.h"

#include "game/native/KernelModuleCatalog.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace lengjing::game::native {
namespace {

extern "C" {
extern const unsigned char _binary_kernel_510245_start[];
extern const unsigned char _binary_kernel_510245_end[];
extern const unsigned char _binary_kernel_510252_start[];
extern const unsigned char _binary_kernel_510252_end[];
extern const unsigned char _binary_kernel_515202_start[];
extern const unsigned char _binary_kernel_515202_end[];
extern const unsigned char _binary_kernel_515202a13_start[];
extern const unsigned char _binary_kernel_515202a13_end[];
extern const unsigned char _binary_kernel_61166_start[];
extern const unsigned char _binary_kernel_61166_end[];
extern const unsigned char _binary_kernel_66127_start[];
extern const unsigned char _binary_kernel_66127_end[];
extern const unsigned char _binary_kernel_61276_start[];
extern const unsigned char _binary_kernel_61276_end[];
}

constexpr unsigned long kDriverHandshakeA = 0x7A3B9C4DUL;
constexpr unsigned long kDriverHandshakeB = 0x2F8E1D6BUL;

struct EmbeddedModule final {
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;

    std::size_t Size() const noexcept {
        const std::uintptr_t first =
            reinterpret_cast<std::uintptr_t>(begin);
        const std::uintptr_t last =
            reinterpret_cast<std::uintptr_t>(end);
        return first != 0 && last >= first
            ? static_cast<std::size_t>(last - first)
            : 0;
    }

    bool Matches(std::string_view release) const {
        const std::size_t size = Size();
        if (size < 64 ||
            begin[0] != 0x7F ||
            begin[1] != 'E' ||
            begin[2] != 'L' ||
            begin[3] != 'F') {
            return false;
        }
        const std::string marker = "vermagic=" + std::string(release) + " ";
        return std::search(
                   begin,
                   begin + size,
                   marker.begin(),
                   marker.end()) != begin + size;
    }
};

EmbeddedModule FindEmbeddedModule(KernelModuleId module) noexcept {
    switch (module) {
        case KernelModuleId::Kernel510245:
            return {_binary_kernel_510245_start, _binary_kernel_510245_end};
        case KernelModuleId::Kernel510252:
            return {_binary_kernel_510252_start, _binary_kernel_510252_end};
        case KernelModuleId::Kernel515202:
            return {_binary_kernel_515202_start, _binary_kernel_515202_end};
        case KernelModuleId::Kernel515202Android13:
            return {
                _binary_kernel_515202a13_start,
                _binary_kernel_515202a13_end};
        case KernelModuleId::Kernel61166:
            return {_binary_kernel_61166_start, _binary_kernel_61166_end};
        case KernelModuleId::Kernel66127:
            return {_binary_kernel_66127_start, _binary_kernel_66127_end};
        case KernelModuleId::Kernel61276:
            return {_binary_kernel_61276_start, _binary_kernel_61276_end};
    }
    return {};
}

bool ProbeKernelDriver() noexcept {
    int descriptor = -1;
    syscall(
        SYS_reboot,
        kDriverHandshakeA,
        kDriverHandshakeB,
        0UL,
        &descriptor);
    if (descriptor < 0) return false;
    close(descriptor);
    return true;
}

std::string CurrentKernelRelease() {
    utsname info{};
    if (uname(&info) != 0) return {};
    return info.release;
}

std::string ErrnoMessage(int value) {
    const char* text = std::strerror(value);
    return text != nullptr ? std::string(text) : std::string("unknown error");
}

}  // namespace

bool EnsureKernelDriverReady(std::string& error) {
    error.clear();
    if (ProbeKernelDriver()) return true;

    const std::string release = CurrentKernelRelease();
    if (release.empty()) {
        error = "无法读取设备完整内核版本";
        return false;
    }

    const KernelModuleRelease* selected = FindKernelModuleRelease(release);
    if (selected == nullptr) {
        error = "没有与设备内核版本完全匹配的内核模块: " + release;
        return false;
    }

    const EmbeddedModule module = FindEmbeddedModule(selected->module);
    if (!module.Matches(release)) {
        error = "内置内核模块资源无效: " + release;
        return false;
    }

    errno = 0;
    const long result = syscall(
        SYS_init_module,
        module.begin,
        module.Size(),
        "");
    const int loadError = errno;
    if (result != 0 && loadError != EEXIST) {
        error = "内核模块加载失败 (" + release + "): " +
            ErrnoMessage(loadError);
        return false;
    }

    if (!ProbeKernelDriver()) {
        error = "内核模块已处理但接口探测失败: " + release;
        return false;
    }
    return true;
}

}  // namespace lengjing::game::native
