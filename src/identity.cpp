#include "sshforum/identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace sshforum {
namespace {

constexpr std::string_view kDomain = "sshforum/identity/v1";
constexpr char kHex[] = "0123456789abcdef";
constexpr char kBase32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

struct CleansedDigest {
    std::array<unsigned char, 32> bytes{};

    ~CleansedDigest() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

void update(EVP_MAC_CTX* context, const unsigned char* data, std::size_t size) {
    if (EVP_MAC_update(context, data, size) != 1) {
        throw std::runtime_error("could not compute author identity");
    }
}

void update_field(EVP_MAC_CTX* context, std::string_view field) {
    std::array<unsigned char, 8> length{};
    const auto size = static_cast<std::uint64_t>(field.size());
    for (std::size_t i = 0; i < length.size(); ++i) {
        length[length.size() - 1 - i] = static_cast<unsigned char>(size >> (8 * i));
    }
    update(context, length.data(), length.size());
    if (!field.empty()) {
        update(context, reinterpret_cast<const unsigned char*>(field.data()), field.size());
    }
}

int hex_value(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return -1;
}

}  // namespace

std::string generate_identity_secret() {
    std::string secret(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(secret.data()),
                   static_cast<int>(secret.size())) != 1) {
        OPENSSL_cleanse(secret.data(), secret.size());
        throw std::runtime_error("could not generate identity secret");
    }
    return secret;
}

std::string make_author_id(std::string_view secret, std::string_view peer_ip,
                           std::string_view username, std::string_view auth_method,
                           std::string_view credential) {
    if (secret.size() != 32) {
        throw std::invalid_argument("identity secret must be 32 bytes");
    }

    std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> mac(
        EVP_MAC_fetch(nullptr, "HMAC", nullptr), &EVP_MAC_free);
    if (!mac) {
        throw std::runtime_error("could not compute author identity");
    }
    std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> context(
        EVP_MAC_CTX_new(mac.get()), &EVP_MAC_CTX_free);
    if (!context) {
        throw std::runtime_error("could not compute author identity");
    }

    char digest_name[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0),
        OSSL_PARAM_construct_end(),
    };
    if (EVP_MAC_init(context.get(),
                     reinterpret_cast<const unsigned char*>(secret.data()),
                     secret.size(), params) != 1) {
        throw std::runtime_error("could not compute author identity");
    }

    update(context.get(), reinterpret_cast<const unsigned char*>(kDomain.data()),
           kDomain.size());
    update_field(context.get(), peer_ip);
    update_field(context.get(), username);
    update_field(context.get(), auth_method);
    update_field(context.get(), credential);

    CleansedDigest digest;
    std::size_t digest_size = 0;
    if (EVP_MAC_final(context.get(), digest.bytes.data(), &digest_size,
                      digest.bytes.size()) != 1 || digest_size != digest.bytes.size()) {
        throw std::runtime_error("could not compute author identity");
    }

    std::string author_id = "v1:";
    author_id.reserve(3 + digest_size * 2);
    for (unsigned char byte : digest.bytes) {
        author_id.push_back(kHex[byte >> 4]);
        author_id.push_back(kHex[byte & 0x0f]);
    }
    return author_id;
}

std::string display_author(std::string_view author_id) {
    if (author_id.size() != 67 || author_id.substr(0, 3) != "v1:") {
        return "Anonymous";
    }
    for (char digit : author_id.substr(3)) {
        if (hex_value(digit) < 0) return "Anonymous";
    }

    std::uint64_t first_five = 0;
    for (std::size_t i = 3; i < 13; i += 2) {
        first_five = (first_five << 8) |
                     static_cast<unsigned int>((hex_value(author_id[i]) << 4) |
                                               hex_value(author_id[i + 1]));
    }
    std::string display = "Anonymous#";
    display.reserve(18);
    for (int shift = 35; shift >= 0; shift -= 5) {
        display.push_back(kBase32[(first_five >> shift) & 0x1f]);
    }
    return display;
}

}  // namespace sshforum
