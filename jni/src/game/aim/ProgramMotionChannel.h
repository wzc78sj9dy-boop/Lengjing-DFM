#pragma once

#include <cstdint>
#include <sys/types.h>

namespace lengjing::game::aim {

class ProgramMotionChannel final {
public:
    ProgramMotionChannel() = default;
    ~ProgramMotionChannel();

    ProgramMotionChannel(const ProgramMotionChannel&) = delete;
    ProgramMotionChannel& operator=(const ProgramMotionChannel&) = delete;

    bool Start();
    bool Ready() const noexcept;
    bool Send(float pitch, float yaw);
    bool Release() noexcept;
    void Close() noexcept;

private:
    bool Resolve(pid_t processId);
    bool WriteCommand(bool enabled, float pitch, float yaw) const;

    pid_t processId_ = -1;
    std::uintptr_t commandAddress_ = 0;
    bool ready_ = false;
};

}  // namespace lengjing::game::aim
