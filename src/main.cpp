#include "sshforum/server.hpp"
#include <charconv>
#include <clocale>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
int number(std::string_view value, int maximum) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < 1 || result > maximum)
        throw std::invalid_argument("invalid numeric option: " + std::string(value));
    return result;
}
}
int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    try {
        sshforum::ServerOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                std::cout << "sshforum - anonymous SSH forum\n"
                             "  --bind ADDRESS       Listen address (0.0.0.0)\n"
                             "  --port PORT          SSH port (2222)\n"
                             "  --db PATH            SQLite database (data/forum.db)\n"
                             "  --host-key PATH      Persistent Ed25519 host key (generated if absent)\n"
                             "  --max-sessions N     Concurrent connections (128)\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + std::string(arg));
            const std::string value(argv[++i]);
            if (arg == "--bind") options.bind = value;
            else if (arg == "--port") options.port = number(value, 65535);
            else if (arg == "--db") options.database = value;
            else if (arg == "--host-key") options.host_key = value;
            else if (arg == "--max-sessions") options.max_sessions = number(value, 4096);
            else throw std::invalid_argument("unknown option: " + std::string(arg));
        }
        return sshforum::run_server(options);
    } catch (const std::exception& error) {
        std::cerr << "sshforum: " << error.what() << '\n';
        return 1;
    }
}
