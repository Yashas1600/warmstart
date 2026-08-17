// main.cpp — warmstart entry point.

#include "launcher.h"
#include <iostream>

int main(int argc, char* argv[]) {
    try {
        auto opts = ws::parse_args(argc, argv);
        return ws::run(opts);
    } catch (const std::exception& e) {
        std::cerr << "[warmstart] Fatal: " << e.what() << "\n";
        return 1;
    }
}
