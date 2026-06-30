#include "cli.hpp"
#include "logging.hpp"

#include <exception>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    std::vector<std::string> args(argv, argv + argc);
    try {
        return apd::cli::run(args);
    } catch (const std::exception &e) {
        apd::log::error("Error: {}", e.what());
        return 1;
    }
}
