#include "platform/BackgroundProcess.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

namespace lengjing::platform {
namespace {

bool RedirectStandardStreams() noexcept {
    const int nullDescriptor = open("/dev/null", O_RDWR);
    if (nullDescriptor < 0) return false;

    const bool redirected =
        dup2(nullDescriptor, STDIN_FILENO) >= 0 &&
        dup2(nullDescriptor, STDOUT_FILENO) >= 0 &&
        dup2(nullDescriptor, STDERR_FILENO) >= 0;
    if (nullDescriptor > STDERR_FILENO) close(nullDescriptor);
    return redirected;
}

bool WriteReady(int descriptor) noexcept {
    constexpr char kReady = 1;
    ssize_t written = -1;
    do {
        written = write(descriptor, &kReady, sizeof(kReady));
    } while (written < 0 && errno == EINTR);
    return written == sizeof(kReady);
}

}  // namespace

bool DetachFromTerminal() noexcept {
    int readyPipe[2] = {-1, -1};
    if (pipe(readyPipe) != 0) return false;

    const pid_t child = fork();
    if (child < 0) {
        close(readyPipe[0]);
        close(readyPipe[1]);
        return false;
    }

    if (child > 0) {
        close(readyPipe[1]);
        char ready = 0;
        ssize_t size = -1;
        do {
            size = read(readyPipe[0], &ready, sizeof(ready));
        } while (size < 0 && errno == EINTR);
        close(readyPipe[0]);
        _exit(size == sizeof(ready) && ready == 1 ? 0 : 1);
    }

    close(readyPipe[0]);
    prctl(PR_SET_PDEATHSIG, 0);
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);

    if (setsid() < 0 || !RedirectStandardStreams() || chdir("/") != 0) {
        close(readyPipe[1]);
        _exit(1);
    }

    const bool notified = WriteReady(readyPipe[1]);
    close(readyPipe[1]);
    if (!notified) _exit(1);
    return true;
}

}  // namespace lengjing::platform
