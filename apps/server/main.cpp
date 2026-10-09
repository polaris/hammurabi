#include <CLI/CLI.hpp>
#include <hammurabi/version.h>

#include <iostream>
#include <print>

int main(int argc, char** argv) {
    try {
        CLI::App app{"Hammurabi server"};
        argv = app.ensure_utf8(argv);

        app.add_flag_callback(
            "--version",
            [] {
                std::println("hammurabi-server {}", hammurabi::version);
                throw CLI::Success{};
            },
            "Display program version information and exit");

        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& e) {
            return app.exit(e);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}