#pragma once
#include <string>

namespace sshforum {
struct ServerOptions {
    std::string bind = "0.0.0.0";
    int port = 2222;
    std::string database = "data/forum.db";
    std::string host_key = "data/ssh_host_ed25519_key";
    int max_sessions = 128;
};
int run_server(const ServerOptions& options);
} // namespace sshforum
