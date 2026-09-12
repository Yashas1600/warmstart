// main.cpp — warmstart entry point.

#include "launcher.h"
#include <iostream>
#include <unistd.h>

int main(int argc, char* argv[]) {
#ifndef __linux__
    std::cerr << "[warmstart] Fatal: this tool requires Linux "
                 "(namespaces, cgroups v2, pivot_root).\n"
              << "Run inside a Linux VM or Docker container with --privileged.\n";
    return 1;
#else
    if (getuid() != 0) {
        std::cerr << "[warmstart] Warning: not running as root. "
                     "Namespace and cgroup operations may fail.\n";
    }

    try {
        auto opts = ws::parse_args(argc, argv);
        return ws::run(opts);
    } catch (const std::exception& e) {
        std::cerr << "[warmstart] Fatal: " << e.what() << "\n";
        return 1;
    }
#endif
}
