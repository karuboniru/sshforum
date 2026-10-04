#include "sshforum/identity.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Exception, typename Function>
void require_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::string fixed_secret() {
    std::string secret;
    for (int i = 0; i < 32; ++i) secret.push_back(static_cast<char>(i));
    return secret;
}

void test_known_vectors_and_display() {
    // Independently calculated with Python hashlib/hmac. The message is the
    // ASCII domain followed by four 8-byte big-endian lengths and raw fields.
    const auto secret = fixed_secret();
    const auto id = sshforum::make_author_id(secret, "203.0.113.7", "alice",
                                             "password", "hunter2");
    require(id == "v1:d1df1f72ff58071d9edf7fa39655d344ae9991771a36c84b6ef63455aca0b193",
            "HMAC-SHA256 should match the independent known vector");
    require(sshforum::display_author(id) == "Anonymous#2HPR64X7",
            "display label should encode the first five digest bytes");
    require(sshforum::display_author("v1:68656c6c6f" + std::string(54, '0')) ==
                "Anonymous#NBSWY3DP",
            "display label should match the RFC 4648 Base32 hello vector");

    const std::string ip("\0\xff", 2);
    const std::string username("a\0b", 3);
    const std::string credential("\0\x01\xff", 3);
    require(sshforum::make_author_id(secret, ip, username, "publickey", credential) ==
                "v1:8d7f27d28d1d2c0be3e270483a51747d2e7b0fd90cbc5b5fa7892250dc64ba78",
            "embedded NUL and non-ASCII bytes should be part of the HMAC input");
}

void test_stability_and_field_separation() {
    const auto secret = fixed_secret();
    const auto baseline = sshforum::make_author_id(secret, "203.0.113.7", "alice",
                                                   "password", "hunter2");
    require(baseline == sshforum::make_author_id(secret, "203.0.113.7", "alice",
                                                 "password", "hunter2"),
            "equal inputs should produce stable IDs");
    require(baseline != sshforum::make_author_id(secret, "203.0.113.8", "alice",
                                                 "password", "hunter2"),
            "peer IP should affect the ID");
    require(baseline != sshforum::make_author_id(secret, "203.0.113.7", "bob",
                                                 "password", "hunter2"),
            "username should affect the ID");
    require(baseline != sshforum::make_author_id(secret, "203.0.113.7", "alice",
                                                 "publickey", "hunter2"),
            "authentication method should affect the ID");
    require(baseline != sshforum::make_author_id(secret, "203.0.113.7", "alice",
                                                 "password", "different"),
            "credential should affect the ID");
    auto other_secret = secret;
    other_secret[0] ^= 1;
    require(baseline != sshforum::make_author_id(other_secret, "203.0.113.7", "alice",
                                                 "password", "hunter2"),
            "secret should affect the ID");

    const auto split_one = sshforum::make_author_id(secret, "a", "bc", "password", "x");
    const auto split_two = sshforum::make_author_id(secret, "ab", "c", "password", "x");
    require(split_one ==
                "v1:db80b02d90860016fa5f5a6d41b557e5f74d93a8465645bb75981ce7cb3f9bdb",
            "first field-boundary vector should match");
    require(split_two ==
                "v1:e9cff428986ee79501fca11a8694eeada61858a65b022132048c7d82a6149e8b",
            "second field-boundary vector should match");
    require(split_one != split_two, "different field boundaries must not collide");
    require(baseline != sshforum::make_author_id(secret, "203.0.113.7", "alice",
                                                 "password", std::string_view("hunter2\0", 8)),
            "trailing NUL should change credential bytes");
}

void test_invalid_inputs_and_random_secret() {
    require_throws<std::invalid_argument>(
        [] { sshforum::make_author_id("", "ip", "name", "password", "value"); },
        "empty secret should be rejected");
    require_throws<std::invalid_argument>(
        [] { sshforum::make_author_id(std::string(31, 'x'), "ip", "name", "password", "value"); },
        "short secret should be rejected");
    require_throws<std::invalid_argument>(
        [] { sshforum::make_author_id(std::string(33, 'x'), "ip", "name", "password", "value"); },
        "long secret should be rejected");

    const auto first = sshforum::generate_identity_secret();
    const auto second = sshforum::generate_identity_secret();
    require(first.size() == 32 && second.size() == 32,
            "generated secrets should contain exactly 32 raw bytes");
    require(first != second, "independently generated secrets should differ");

    const auto valid = sshforum::make_author_id(fixed_secret(), "ip", "user",
                                                 "password", "value");
    require(sshforum::display_author("") == "Anonymous", "empty author ID should be anonymous");
    require(sshforum::display_author("password:secret") == "Anonymous",
            "unknown author ID should not reveal source text");
    require(sshforum::display_author("v2:" + valid.substr(3)) == "Anonymous",
            "unknown version should be anonymous");
    require(sshforum::display_author(valid.substr(0, valid.size() - 1)) == "Anonymous",
            "truncated author ID should be anonymous");
    require(sshforum::display_author(valid + "0") == "Anonymous",
            "extra characters should make an author ID invalid");
    auto uppercase = valid;
    uppercase[3] = 'F';
    require(sshforum::display_author(uppercase) == "Anonymous",
            "uppercase hex should be rejected");
    auto nonhex = valid;
    nonhex.back() = 'g';
    require(sshforum::display_author(nonhex) == "Anonymous",
            "nonhex digest bytes should be rejected");
}

}  // namespace

int main() {
    try {
        test_known_vectors_and_display();
        test_stability_and_field_separation();
        test_invalid_inputs_and_random_secret();
        std::cout << "identity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "identity tests failed: " << error.what() << '\n';
        return 1;
    }
}
