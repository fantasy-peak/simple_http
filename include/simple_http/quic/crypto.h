#pragma once

// QUIC packet protection (RFC 9001): the keys each encryption level uses, the
// AEAD that seals and opens a packet, and the header protection that hides the
// packet number the AEAD authenticates.
//
// This is a layer of functions over byte strings, not an object with a life of
// its own. The connection layer owns *when* a key is derived and which
// encryption level it belongs to; what is left over is arithmetic on secrets,
// and keeping that arithmetic free-standing means the key schedule can be
// tested against the RFC vectors without a connection in sight. The one piece
// of state that does live here — `PacketKeys` — is a plain, copyable value,
// because the send and receive halves of a connection both hold a key for each
// level and each key update swaps one out from under the other.
//
// OpenSSL 3.5 EVP only: AEAD through `EVP_CIPHER`, the key schedule through the
// `EVP_KDF` "HKDF" provider. The low-level SHA/HMAC/`HKDF_expand` entry points
// are deprecated or FIPS-internal, and reaching for them would buy a warning
// and a provider dependency in exchange for nothing this file needs.
//
// Two RFC 9001 rules shape the interface:
//   * a packet that does not authenticate is dropped, never partly processed —
//     `aead_open` returns false and the caller loses the datagram;
//   * header protection is removed *before* the packet number can be read and
//     applied *after* it is written, because the mask and the packet number
//     occupy the same octets. That is why `remove_header_protection` reports
//     the packet-number length it just uncovered: there is no way to decode the
//     number without first unmasking it.

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include "wire.h"

namespace simple_http::quic {

// The four QUIC encryption levels, in the order keys become available.
enum class EncryptionLevel : std::uint8_t { Initial = 0, ZeroRtt = 1, Handshake = 2, OneRtt = 3 };
inline constexpr std::size_t kEncryptionLevelCount = 4;

// The authentication tag every protected packet ends with (RFC 9001 §5.3). All
// three AEADs QUIC permits use 16 octets, so a reader that needs the end of the
// payload without decrypting it subtracts this one number.
inline constexpr std::size_t kAeadTagLen = 16;

// A nonce and a header-protection mask. Named types rather than bare arrays
// because both cross function boundaries, and a C array parameter decays to a
// pointer — taking its length with it. An off-by-one in either is silent: the
// packet still encrypts, it just decrypts to something else.
inline constexpr std::size_t kNonceLen = 12;
inline constexpr std::size_t kHeaderProtectionMaskLen = 5;
using Nonce = std::array<unsigned char, kNonceLen>;
using HeaderProtectionMask = std::array<std::uint8_t, kHeaderProtectionMaskLen>;

// One direction's keys at one encryption level.
struct PacketKeys {
    bool valid{false};
    const EVP_MD* md{nullptr};        // SHA-256 for Initial, negotiated hash after
    const EVP_CIPHER* aead{nullptr};  // AES-128-GCM / AES-256-GCM / ChaCha20-Poly1305
    const EVP_CIPHER* hp{nullptr};    // AES-128-ECB / AES-256-ECB, or ChaCha20
    std::string key;                  // AEAD key
    std::string iv;                   // 12 octets, XORed with the packet number
    std::string hp_key;               // header protection key
    std::string secret;               // retained so the key can be updated
};

// HKDF-Expand-Label (RFC 8446 §7.1), over OpenSSL's HKDF.
std::string hkdf_expand_label(std::string_view secret, std::string_view label, std::string_view context,
                              std::size_t out_len, const EVP_MD* md);

// The Initial secrets, derived from the client's *destination* connection ID
// with the fixed salt (RFC 9001 §5.2). Returns {client_secret, server_secret}.
//
// These are the per-direction secrets — HKDF-Expand-Label(initial_secret,
// "client in" / "server in") — and not the common `initial_secret` the RFC
// prints alongside them, because each direction's keys come from its own and
// the two are never interchangeable.
std::pair<std::string, std::string> initial_secrets(std::string_view dcid);

// Initial keys, and the suite OpenSSL negotiated (queried from the SSL object).
PacketKeys derive_initial_keys(std::string_view secret);
PacketKeys derive_packet_keys(std::string_view secret, const EVP_MD* md, const EVP_CIPHER* aead);
// Next key phase (RFC 9001 §6): secret = HKDF-Expand-Label(secret, "quic ku", "", hash_len).
PacketKeys next_generation_keys(const PacketKeys& current);

// --- packet protection ---
// AEAD-seal `plaintext` with the packet number, using `aad` as associated data.
// Appends the 16-octet tag to `out` (out is cleared first). False on failure.
bool aead_seal(const PacketKeys& keys, std::uint64_t packet_number, std::string_view aad,
               std::string_view plaintext, std::string& out);
bool aead_open(const PacketKeys& keys, std::uint64_t packet_number, std::string_view aad,
               std::string_view ciphertext, std::string& out);

// The 5-octet header-protection mask (RFC 9001 §5.4.1). `sample` is 16 octets
// taken 4 octets into the packet-number field.
void header_protection_mask(const PacketKeys& keys, std::span<const std::uint8_t> sample,
                            HeaderProtectionMask& out_mask);

// Apply / remove header protection in place. `pn_offset` is the offset of the
// first octet of the packet-number field; the low two bits of packet[0] encode
// its length. `apply` uses `pn_len`; `remove` reads the length out of the
// unmasked first octet and returns it in `pn_len`.
void apply_header_protection(const PacketKeys& keys, std::string& packet, std::size_t pn_offset,
                             std::size_t pn_len);
bool remove_header_protection(const PacketKeys& keys, std::string& packet, std::size_t pn_offset,
                              std::size_t& pn_len);

// The Retry packet's integrity tag (RFC 9001 §5.8). `pseudo_packet` is the
// Retry Pseudo-Packet; the 16-octet tag is appended to `out`.
void retry_integrity_tag(std::string_view pseudo_packet, std::string& out);

// --- implementation -------------------------------------------------------

namespace detail {

// The initial salt, fixed by RFC 9001 §5.2 for QUIC v1 (the literal spells out
// 38762cf7f55934b34d179ae6a4c80cadccbb7f0a). It is not a secret and never
// changes: Initial protection exists to keep a passive observer out, not a peer.
inline constexpr std::string_view kInitialSalt =
    "\x38\x76\x2c\xf7\xf5\x59\x34\xb3\x4d\x17\x9a\xe6\xa4\xc8\x0c\xad\xcc\xbb\x7f\x0a";

// The fixed key and nonce of a Retry's integrity tag (RFC 9001 §5.8). Also not
// a secret — a Retry is sent before any key exists, and what the tag proves is
// that the sender saw the client's original connection ID, which an off-path
// attacker has not.
inline constexpr std::array<unsigned char, 16> kRetryKey{0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a,
                                                          0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e};
inline constexpr std::array<unsigned char, kNonceLen> kRetryNonce{0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63,
                                                                  0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb};

// An EVP_CIPHER_CTX is heap-allocated by OpenSSL but owned by the call, so the
// holder sits on the caller's stack. One per operation, deliberately: the
// contexts are neither copyable nor thread-safe, and `PacketKeys` is passed
// around by value, so a context cached inside it would be free to be used from
// two directions at once.
struct CipherCtx {
    CipherCtx() noexcept : p(EVP_CIPHER_CTX_new()) {}
    ~CipherCtx() { EVP_CIPHER_CTX_free(p); }
    CipherCtx(const CipherCtx&) = delete;
    CipherCtx& operator=(const CipherCtx&) = delete;

    explicit operator bool() const noexcept { return p != nullptr; }

    EVP_CIPHER_CTX* p;
};

// The fetched method object has to outlive the context built from it, so the two
// travel together. Fetching rather than holding a static is on purpose: which
// provider answers depends on the library context the process loaded, and a
// static would freeze the answer at first use.
struct KdfCtx {
    KdfCtx() noexcept : kdf(EVP_KDF_fetch(nullptr, "HKDF", nullptr)) {
        if (kdf != nullptr) ctx = EVP_KDF_CTX_new(kdf);
    }
    ~KdfCtx() {
        EVP_KDF_CTX_free(ctx);
        EVP_KDF_free(kdf);
    }
    KdfCtx(const KdfCtx&) = delete;
    KdfCtx& operator=(const KdfCtx&) = delete;

    explicit operator bool() const noexcept { return ctx != nullptr; }

    EVP_KDF* kdf{nullptr};
    EVP_KDF_CTX* ctx{nullptr};
};

// EVP takes `const unsigned char*` with no length of its own, and a default
// constructed string_view may be null; a Retry seals an empty plaintext, so the
// null case is real and gets a pointer to a single zero octet instead.
inline const unsigned char* as_uchar(std::string_view s) noexcept {
    static constexpr unsigned char kEmpty = 0;
    return s.empty() ? &kEmpty : reinterpret_cast<const unsigned char*>(s.data());
}

// Same problem, worse failure: an OSSL_PARAM built over a null pointer makes the
// provider's get_octet_string fail, so "no salt" and "no context" would turn
// into a key derivation that quietly produces nothing. Zero-length parameters
// still need a real pointer.
inline char* param_ptr(std::string_view s) noexcept {
    static char kEmpty = '\0';
    return s.empty() ? &kEmpty : const_cast<char*>(s.data());
}

// HKDF-Extract and HKDF-Expand are the same provider in two modes: extract takes
// the input key material in `key` and the salt in `salt`, expand takes the
// pseudorandom key in `key`. Empty on failure — the caller treats that as "no
// keys", which is a connection error, not a special case to handle per call.
//
// RFC 8446's Expand-Label has no provider of its own; it is Expand with a
// structured info field, so `info` is built by the caller and passed through.
inline Bytes hkdf(std::string_view key, std::string_view salt, std::string_view info, bool extract,
                  std::size_t out_len, const EVP_MD* md) {
    if (md == nullptr || out_len == 0) return {};

    detail::KdfCtx kdf;
    if (!kdf) return {};

    // The mode string is the only thing separating the two halves of HKDF, and
    // it is what makes the EXPAND_ONLY call below use `key` as the PRK rather
    // than as input key material.
    const char* mode = extract ? "EXTRACT_ONLY" : "EXPAND_ONLY";
    OSSL_PARAM params[5];
    std::size_t n = 0;
    params[n++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_MODE, const_cast<char*>(mode), 0);
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, param_ptr(key), key.size());
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, param_ptr(salt), salt.size());
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, param_ptr(info), info.size());
    params[n] = OSSL_PARAM_construct_end();

    // Derive takes the parameters itself, so there is no separate set_params
    // step to forget. A missing digest is an error here, not a default: the
    // caller always knows which hash TLS negotiated.
    OSSL_PARAM digest[2];
    digest[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                                 const_cast<char*>(EVP_MD_get0_name(md)), 0);
    digest[1] = OSSL_PARAM_construct_end();

    Bytes out(out_len, '\0');
    if (EVP_KDF_CTX_set_params(kdf.ctx, digest) != 1) return {};
    if (EVP_KDF_derive(kdf.ctx, reinterpret_cast<unsigned char*>(out.data()), out.size(), params) != 1) {
        return {};
    }
    return out;
}

// RFC 9001 §5.3: the nonce is the IV with the packet number XORed into its last
// eight octets, big-endian. The first four octets are left alone because a
// truncated packet number is at most four octets and a nonce is twelve — the
// two only ever overlap in the tail.
inline void make_nonce(const PacketKeys& keys, std::uint64_t packet_number, Nonce& nonce) {
    std::ranges::transform(keys.iv, nonce.begin(),
                           [](char c) { return static_cast<unsigned char>(c); });
    // The packet number goes into the last eight octets, big-endian, so that a
    // truncated number (at most four octets) lands wholly inside them.
    constexpr std::size_t kPnOctets = 8;
    for (std::size_t i = 0; i < kPnOctets; ++i) {
        nonce[kNonceLen - kPnOctets + i] ^=
            static_cast<unsigned char>(packet_number >> (8 * (kPnOctets - 1 - i)));
    }
}

// The AEAD's own length prefix is the useful bound here: EVP takes int lengths,
// and a QUIC packet is never close to 2 GiB, so anything larger is a bug
// upstream and worth refusing rather than truncating.
inline bool fits_int(std::size_t n) noexcept { return n <= static_cast<std::size_t>(INT_MAX); }

// The 16-octet header protection sample, which no header byte overlaps once
// `offset` is four octets past the packet-number field.
inline const std::uint8_t* sample_at(const Bytes& packet, std::size_t offset) noexcept {
    return reinterpret_cast<const std::uint8_t*>(packet.data()) + offset;
}

}  // namespace detail

inline std::string hkdf_expand_label(std::string_view secret, std::string_view label, std::string_view context,
                                     std::size_t out_len, const EVP_MD* md) {
    // HkdfLabel (RFC 8446 §7.1) as a TLS 1.3 opaque vector: two octets of output
    // length, then the label with its leading "tls13 " and a one-octet length,
    // then the context with a one-octet length. The prefix is not decoration —
    // it is what keeps a QUIC label from colliding with a TLS 1.3 handshake
    // label derived from the same secret.
    if (out_len > 0xffff || label.size() + 6 > 0xff || context.size() > 0xff) return {};

    Bytes info;
    append_u16(info, static_cast<std::uint16_t>(out_len));
    append_u8(info, static_cast<std::uint8_t>(label.size() + 6));
    info.append("tls13 ");
    info.append(label);
    append_u8(info, static_cast<std::uint8_t>(context.size()));
    info.append(context);

    return detail::hkdf(secret, {}, info, /*extract=*/false, out_len, md);
}

inline std::pair<std::string, std::string> initial_secrets(std::string_view dcid) {
    // RFC 9001 §5.2: one HKDF-Extract over the client's chosen connection ID
    // gives the initial secret, and the two directions are two Expand-Labels
    // apart. Both peers can compute this without having exchanged anything,
    // which is exactly why it protects the handshake from observers and nobody
    // else.
    const EVP_MD* md = EVP_sha256();
    const Bytes initial = detail::hkdf(dcid, detail::kInitialSalt, {}, /*extract=*/true, 32, md);
    if (initial.size() != 32) return {};

    return {hkdf_expand_label(initial, "client in", {}, 32, md),
            hkdf_expand_label(initial, "server in", {}, 32, md)};
}

inline PacketKeys derive_packet_keys(std::string_view secret, const EVP_MD* md, const EVP_CIPHER* aead) {
    PacketKeys keys;
    if (md == nullptr || aead == nullptr || secret.empty()) return keys;

    // Header protection is a second cipher over the same key material, and which
    // one is decided by the AEAD, not negotiated separately (RFC 9001 §5.4):
    // AES-GCM hides the packet number with one raw AES block, ChaCha20-Poly1305
    // with a ChaCha20 keystream.
    const EVP_CIPHER* hp = nullptr;
    if (EVP_CIPHER_is_a(aead, "AES-128-GCM")) {
        hp = EVP_aes_128_ecb();
    } else if (EVP_CIPHER_is_a(aead, "AES-256-GCM")) {
        hp = EVP_aes_256_ecb();
    } else if (EVP_CIPHER_is_a(aead, "CHACHA20-POLY1305")) {
        hp = EVP_chacha20();
    } else {
        return keys;
    }

    // Each label's output length is fixed by the algorithm, never by the caller:
    // the key is the AEAD's key size, the IV is always 12 octets (§5.3), and the
    // header protection key is the width of the block it encrypts — which is the
    // AES key size, or ChaCha20's 32 octets.
    const std::size_t key_len = static_cast<std::size_t>(EVP_CIPHER_get_key_length(aead));
    const std::size_t hp_len = static_cast<std::size_t>(EVP_CIPHER_get_key_length(hp));

    const std::string key = hkdf_expand_label(secret, "quic key", {}, key_len, md);
    const std::string iv = hkdf_expand_label(secret, "quic iv", {}, 12, md);
    const std::string hp_key = hkdf_expand_label(secret, "quic hp", {}, hp_len, md);
    if (key.size() != key_len || iv.size() != 12 || hp_key.size() != hp_len) return keys;

    keys.valid = true;
    keys.md = md;
    keys.aead = aead;
    keys.hp = hp;
    keys.key = key;
    keys.iv = iv;
    keys.hp_key = hp_key;
    // Retained only for `next_generation_keys`: the update label is applied to
    // the secret, not to the derived key, so the secret has to outlive the
    // derivation that produced this struct.
    keys.secret.assign(secret);
    return keys;
}

inline PacketKeys derive_initial_keys(std::string_view secret) {
    // Initial packets are always AES-128-GCM over SHA-256, whatever the
    // handshake later negotiates (RFC 9001 §5.2) — there is no other option to
    // pick, so there is no negotiation to wait for.
    return derive_packet_keys(secret, EVP_sha256(), EVP_aes_128_gcm());
}

inline PacketKeys next_generation_keys(const PacketKeys& current) {
    // RFC 9001 §6: the key update secret is one Expand-Label over the old
    // secret, and key, IV and hp are then re-derived from it with the *same*
    // algorithm. The old keys stay valid until the peer is seen moving to the
    // new phase, which is the caller's problem, not this function's.
    if (!current.valid || current.md == nullptr) return {};

    const int hash_len = EVP_MD_get_size(current.md);
    if (hash_len <= 0) return {};

    const std::string next =
        hkdf_expand_label(current.secret, "quic ku", {}, static_cast<std::size_t>(hash_len), current.md);
    if (next.empty()) return {};

    return derive_packet_keys(next, current.md, current.aead);
}

inline bool aead_seal(const PacketKeys& keys, std::uint64_t packet_number, std::string_view aad,
                      std::string_view plaintext, std::string& out) {
    out.clear();
    if (!keys.valid || keys.aead == nullptr || keys.iv.size() != 12 || !detail::fits_int(plaintext.size()) ||
        !detail::fits_int(aad.size())) {
        return false;
    }

    Nonce nonce{};
    detail::make_nonce(keys, packet_number, nonce);

    detail::CipherCtx ctx;
    if (!ctx) return false;
    if (EVP_EncryptInit_ex2(ctx.p, keys.aead, nullptr, nullptr, nullptr) != 1) return false;
    if (EVP_EncryptInit_ex2(ctx.p, nullptr, detail::as_uchar(keys.key), nonce.data(), nullptr) != 1) return false;

    // AAD first, with a null output: it is authenticated, never encrypted, so
    // the header travels beside the ciphertext rather than through it.
    int len = 0;
    if (!aad.empty() &&
        EVP_EncryptUpdate(ctx.p, nullptr, &len, detail::as_uchar(aad), static_cast<int>(aad.size())) != 1) {
        return false;
    }

    // The ciphertext is at most the plaintext plus one block; GCM and
    // ChaCha20-Poly1305 have no padding, so in practice it is exactly the
    // plaintext and this is one allocation per packet either way.
    Bytes body(plaintext.size() + EVP_MAX_BLOCK_LENGTH, '\0');
    if (EVP_EncryptUpdate(ctx.p, reinterpret_cast<unsigned char*>(body.data()), &len,
                          detail::as_uchar(plaintext), static_cast<int>(plaintext.size())) != 1) {
        return false;
    }
    std::size_t total = static_cast<std::size_t>(len);
    if (EVP_EncryptFinal_ex(ctx.p, reinterpret_cast<unsigned char*>(body.data()) + total, &len) != 1) {
        return false;
    }
    total += static_cast<std::size_t>(len);

    // The tag goes straight into the string's own tail: resize has already
    // allocated it, so the ctrl writes into live memory rather than into a
    // separate buffer that would be copied once more.
    out.assign(body.data(), total);
    out.resize(total + kAeadTagLen);
    if (EVP_CIPHER_CTX_ctrl(ctx.p, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kAeadTagLen),
                            out.data() + total) != 1) {
        out.clear();
        return false;
    }
    return true;
}

inline bool aead_open(const PacketKeys& keys, std::uint64_t packet_number, std::string_view aad,
                      std::string_view ciphertext, std::string& out) {
    out.clear();
    if (!keys.valid || keys.aead == nullptr || keys.iv.size() != 12 || ciphertext.size() < kAeadTagLen ||
        !detail::fits_int(aad.size())) {
        return false;
    }

    const std::size_t body_len = ciphertext.size() - kAeadTagLen;

    Nonce nonce{};
    detail::make_nonce(keys, packet_number, nonce);

    // The tag is copied out rather than pointed at: the ctrl takes a void* and
    // the ciphertext is const, and a 16-octet copy is cheaper than the cast that
    // would otherwise be needed to lie about that.
    std::array<unsigned char, kAeadTagLen> tag{};
    std::ranges::transform(ciphertext.substr(body_len, kAeadTagLen), tag.begin(),
                           [](char c) { return static_cast<unsigned char>(c); });

    detail::CipherCtx ctx;
    if (!ctx) return false;
    if (EVP_DecryptInit_ex2(ctx.p, keys.aead, nullptr, nullptr, nullptr) != 1) return false;
    if (EVP_DecryptInit_ex2(ctx.p, nullptr, detail::as_uchar(keys.key), nonce.data(), nullptr) != 1) return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.p, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kAeadTagLen), tag.data()) != 1) {
        return false;
    }

    int len = 0;
    if (!aad.empty() &&
        EVP_DecryptUpdate(ctx.p, nullptr, &len, detail::as_uchar(aad), static_cast<int>(aad.size())) != 1) {
        return false;
    }

    Bytes body(body_len + EVP_MAX_BLOCK_LENGTH, '\0');
    if (EVP_DecryptUpdate(ctx.p, reinterpret_cast<unsigned char*>(body.data()), &len,
                          detail::as_uchar(ciphertext.substr(0, body_len)), static_cast<int>(body_len)) != 1) {
        return false;
    }
    std::size_t total = static_cast<std::size_t>(len);

    // Final is where the tag is checked; a forged or replayed packet fails here
    // and nowhere earlier, so nothing above this line may consume `body`.
    if (EVP_DecryptFinal_ex(ctx.p, reinterpret_cast<unsigned char*>(body.data()) + total, &len) != 1) {
        return false;
    }
    total += static_cast<std::size_t>(len);

    out.assign(body.data(), total);
    return true;
}

inline void header_protection_mask(const PacketKeys& keys, std::span<const std::uint8_t> sample,
                                   HeaderProtectionMask& out_mask) {
    out_mask.fill(0);
    // The sample is sixteen octets (RFC 9001 §5.4.2); a shorter one has nothing
    // to mask with, so the mask stays zero rather than reading past it.
    constexpr std::size_t kSampleLen = 16;
    if (!keys.valid || keys.hp == nullptr || sample.size() < kSampleLen) return;

    detail::CipherCtx ctx;
    if (!ctx) return;

    std::array<unsigned char, 16> block{};
    int len = 0;
    if (EVP_CIPHER_is_a(keys.hp, "CHACHA20")) {
        // RFC 9001 §5.4.4: the sample is the initial ChaCha20 counter block —
        // four octets of little-endian counter, then the twelve-octet nonce.
        // OpenSSL's IV for ChaCha20 is that same 16-octet layout, so the sample
        // is passed through verbatim, and the mask is the keystream over zeros.
        if (keys.hp_key.size() != 32) return;
        const std::array<unsigned char, kHeaderProtectionMaskLen> zeros{};
        if (EVP_EncryptInit_ex2(ctx.p, EVP_chacha20(), detail::as_uchar(keys.hp_key), sample.data(),
                                nullptr) != 1) {
            return;
        }
        if (EVP_EncryptUpdate(ctx.p, block.data(), &len, zeros.data(), static_cast<int>(kSampleLen)) != 1) return;
    } else {
        // RFC 9001 §5.4.3: one block of AES-ECB over the sample, no padding —
        // the only block cipher mode QUIC's header protection uses.
        if (keys.hp_key.size() != static_cast<std::size_t>(EVP_CIPHER_get_key_length(keys.hp))) return;
        if (EVP_EncryptInit_ex2(ctx.p, keys.hp, detail::as_uchar(keys.hp_key), nullptr, nullptr) != 1) {
            return;
        }
        if (EVP_CIPHER_CTX_set_padding(ctx.p, 0) != 1) return;
        if (EVP_EncryptUpdate(ctx.p, block.data(), &len, sample.data(), static_cast<int>(kSampleLen)) != 1) return;
    }

    std::ranges::copy_n(block.begin(), kHeaderProtectionMaskLen, out_mask.begin());
}

inline void apply_header_protection(const PacketKeys& keys, std::string& packet, std::size_t pn_offset,
                                    std::size_t pn_len) {
    if (pn_len < 1 || pn_len > kMaxPacketNumberLen) return;
    if (pn_offset + 4 + 16 > packet.size()) return;

    // RFC 9001 §5.4.1: the sample begins four octets into the packet-number
    // field, not at its end, so a short packet number reads into the payload —
    // which is why the sender pads a small packet up to a protectable size.
    HeaderProtectionMask mask{};
    // Sixteen octets taken four octets into the packet-number field
    // (RFC 9001 §5.4.2). Those two numbers belong in one place, and this is it.
    header_protection_mask(
        keys, std::span<const std::uint8_t>(detail::sample_at(packet, pn_offset + 4), 16), mask);

    // A long header's top four bits are the form and the packet type, so only
    // the low four are masked; a short header's low five are two reserved bits,
    // the key phase and the packet-number length, all of which are protected.
    // Masking that spare bit of a long header would rewrite the packet type.
    const bool long_header = (static_cast<std::uint8_t>(packet[0]) & 0x80) != 0;
    const auto first = static_cast<std::uint8_t>(packet[0]);
    packet[0] = static_cast<char>(first ^ (mask[0] & (long_header ? 0x0f : 0x1f)));
    for (std::size_t i = 0; i < pn_len; ++i) {
        const auto b = static_cast<std::uint8_t>(packet[pn_offset + i]);
        packet[pn_offset + i] = static_cast<char>(b ^ mask[1 + i]);
    }
}

inline bool remove_header_protection(const PacketKeys& keys, std::string& packet, std::size_t pn_offset,
                                     std::size_t& pn_len) {
    if (pn_offset + 4 + 16 > packet.size()) return false;

    // The sample comes from the payload, which header protection leaves alone,
    // so it can be taken before anything is unmasked.
    HeaderProtectionMask mask{};
    // Sixteen octets taken four octets into the packet-number field
    // (RFC 9001 §5.4.2). Those two numbers belong in one place, and this is it.
    header_protection_mask(
        keys, std::span<const std::uint8_t>(detail::sample_at(packet, pn_offset + 4), 16), mask);

    const bool long_header = (static_cast<std::uint8_t>(packet[0]) & 0x80) != 0;
    const auto first = static_cast<std::uint8_t>(packet[0]) ^ (mask[0] & (long_header ? 0x0f : 0x1f));

    // The length of the packet-number field is only legible once the first
    // octet is unmasked, and the AAD depends on it, so this is the one place
    // where the unmasking has to happen in two steps.
    pn_len = static_cast<std::size_t>(first & 0x03) + 1;
    if (pn_offset + pn_len > packet.size()) return false;

    packet[0] = static_cast<char>(first);
    for (std::size_t i = 0; i < pn_len; ++i) {
        const auto b = static_cast<std::uint8_t>(packet[pn_offset + i]);
        packet[pn_offset + i] = static_cast<char>(b ^ mask[1 + i]);
    }
    return true;
}

inline void retry_integrity_tag(std::string_view pseudo_packet, std::string& out) {
    out.clear();

    // RFC 9001 §5.8: AES-128-GCM over an empty plaintext with the pseudo-packet
    // as associated data, so the output is nothing but the tag. Packet number
    // zero is not an accident — it leaves the IV unmodified, which is exactly
    // the Retry nonce, and it lets this share the sealing path every other
    // packet uses instead of open-coding a second GCM invocation.
    PacketKeys keys;
    keys.valid = true;
    keys.aead = EVP_aes_128_gcm();
    keys.key.assign(reinterpret_cast<const char*>(detail::kRetryKey.data()), detail::kRetryKey.size());
    keys.iv.assign(reinterpret_cast<const char*>(detail::kRetryNonce.data()), detail::kRetryNonce.size());
    (void)aead_seal(keys, 0, pseudo_packet, {}, out);
}

}  // namespace simple_http::quic
