#include "test_support.h"

#include <exception>
#include <iostream>

int main() {
    try {
        RunPerfExecutionBreakpointTests();
        std::cout << "perf execution breakpoint tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
