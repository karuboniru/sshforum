#pragma once

#include <string>
#include <string_view>

namespace sshforum {

// Returns 32 cryptographically random raw bytes. Persist this secret so an
// installation's author IDs remain stable across restarts.
std::string generate_identity_secret();

// Computes a stable, opaque author ID. The secret must contain exactly 32 raw
// bytes. The HMAC input is "sshforum/identity/v1" followed by each of the four
// fields in order, as an unsigned 64-bit big-endian byte length and raw bytes.
// The complete digest is intended for storage; none of the input fields are.
std::string make_author_id(std::string_view secret, std::string_view peer_ip,
                           std::string_view username, std::string_view auth_method,
                           std::string_view credential);

// Returns a short display label for a valid author ID, or "Anonymous" for
// absent, unknown, or malformed IDs. The short label is not an identity key.
std::string display_author(std::string_view author_id);

}  // namespace sshforum
