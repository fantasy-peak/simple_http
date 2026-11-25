#pragma once

// HTTP/3 SETTINGS that have to survive in a TLS session ticket.
//
// A 0-RTT request is encoded against the SETTINGS the client remembers from an
// earlier connection. If this process now advertises weaker limits (a smaller
// field section, no QPACK dynamic table room, no extended CONNECT, no
// datagrams), replaying the early request could exceed what nghttp3 is willing
// to decode. The server therefore snapshots these values into the ticket and
// refuses early data — while still allowing a full handshake — when the old
// snapshot is not compatible with the current one.
//
// The encoding is deliberately the on-the-wire SETTINGS frame form: it is the
// format quic-go uses, needs no second parser for the values, and keeps the
// snapshot independent of nghttp3's internal struct layout.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nghttp3/nghttp3.h>

#include "../../core/limits.h"

namespace simple_http::h3 {

// HTTP/3's two connection-level knobs that are ours to choose. They live here
// so the live SETTINGS frame and the ticket snapshot cannot drift.
inline constexpr std::uint64_t kQpackMaxTableCapacity = 4096;
inline constexpr std::uint64_t kQpackBlockedStreams = 16;

// One decoded SETTINGS frame, limited to the values that affect 0-RTT.
struct Settings {
    std::uint64_t max_field_section_size{0};
    std::uint64_t qpack_max_dtable_capacity{0};
    std::uint64_t qpack_blocked_streams{0};
    bool datagram{false};
    bool extended_connect{false};
};

inline Settings make_settings(const EngineLimits &limits) {
    Settings settings;
    settings.max_field_section_size = limits.max_header_bytes;
    settings.qpack_max_dtable_capacity = kQpackMaxTableCapacity;
    settings.qpack_blocked_streams = kQpackBlockedStreams;
    return settings;
}

// Ticket appdata prefix. Bump the version when the payload changes shape.
inline constexpr std::string_view kTicketSettingsPrefix = "simple_http h3 settings v1";

namespace detail {

inline void put_uvarint(std::string &out, std::uint64_t value) {
    const std::size_t old = out.size();
    out.resize(old + nghttp3_put_uvarintlen(value));
    (void)nghttp3_put_uvarint(reinterpret_cast<std::uint8_t *>(out.data() + old), value);
}

inline bool get_uvarint(std::string_view in, std::size_t &pos, std::uint64_t &value) {
    if (pos >= in.size())
        return false;
    const auto *p = reinterpret_cast<const std::uint8_t *>(in.data() + pos);
    const std::size_t len = nghttp3_get_uvarintlen(p);
    if (len == 0 || len > in.size() - pos)
        return false;
    (void)nghttp3_get_uvarint(&value, p);
    pos += len;
    return true;
}

inline constexpr std::uint64_t kSettingsFrameType = 0x04;
inline constexpr std::uint64_t kSettingQpackMaxTableCapacity = 0x01;
inline constexpr std::uint64_t kSettingMaxFieldSectionSize = 0x06;
inline constexpr std::uint64_t kSettingQpackBlockedStreams = 0x07;
inline constexpr std::uint64_t kSettingEnableConnectProtocol = 0x08;
inline constexpr std::uint64_t kSettingH3Datagram = 0x33;

} // namespace detail

// Serialize the snapshot as "<prefix><SETTINGS frame>".
inline std::string encode_ticket_settings(const Settings &settings) {
    std::string payload;
    detail::put_uvarint(payload, detail::kSettingQpackMaxTableCapacity);
    detail::put_uvarint(payload, settings.qpack_max_dtable_capacity);
    detail::put_uvarint(payload, detail::kSettingQpackBlockedStreams);
    detail::put_uvarint(payload, settings.qpack_blocked_streams);
    detail::put_uvarint(payload, detail::kSettingMaxFieldSectionSize);
    detail::put_uvarint(payload, settings.max_field_section_size);
    if (settings.datagram) {
        detail::put_uvarint(payload, detail::kSettingH3Datagram);
        detail::put_uvarint(payload, 1);
    }
    if (settings.extended_connect) {
        detail::put_uvarint(payload, detail::kSettingEnableConnectProtocol);
        detail::put_uvarint(payload, 1);
    }

    std::string out{kTicketSettingsPrefix};
    detail::put_uvarint(out, detail::kSettingsFrameType);
    detail::put_uvarint(out, payload.size());
    out.append(payload);
    return out;
}

// True when a request encoded against `ticket` may be replayed under `current`.
//
// Mirrors quic-go's settingsCompatibleFor0RTT, with the QPACK values included:
// an early HEADERS block encoded under a larger dynamic table or with more
// blocked streams than we now advertise must not be accepted.
inline bool ticket_settings_compatible_for_0rtt(std::string_view ticket, const Settings &current) {
    if (ticket.size() < kTicketSettingsPrefix.size() ||
        ticket.substr(0, kTicketSettingsPrefix.size()) != kTicketSettingsPrefix) {
        return false;
    }
    ticket.remove_prefix(kTicketSettingsPrefix.size());

    std::size_t pos = 0;
    std::uint64_t frame_type = 0;
    std::uint64_t frame_length = 0;
    if (!detail::get_uvarint(ticket, pos, frame_type) || frame_type != detail::kSettingsFrameType)
        return false;
    if (!detail::get_uvarint(ticket, pos, frame_length))
        return false;
    if (frame_length != ticket.size() - pos)
        return false;

    Settings old;
    bool seen_qpack_capacity = false;
    bool seen_qpack_blocked = false;
    bool seen_max_field = false;
    bool seen_datagram = false;
    bool seen_extended_connect = false;

    while (pos < ticket.size()) {
        std::uint64_t id = 0;
        std::uint64_t value = 0;
        if (!detail::get_uvarint(ticket, pos, id) || !detail::get_uvarint(ticket, pos, value))
            return false;
        switch (id) {
        case detail::kSettingQpackMaxTableCapacity:
            if (seen_qpack_capacity)
                return false;
            seen_qpack_capacity = true;
            old.qpack_max_dtable_capacity = value;
            break;
        case detail::kSettingQpackBlockedStreams:
            if (seen_qpack_blocked)
                return false;
            seen_qpack_blocked = true;
            old.qpack_blocked_streams = value;
            break;
        case detail::kSettingMaxFieldSectionSize:
            if (seen_max_field)
                return false;
            seen_max_field = true;
            old.max_field_section_size = value;
            break;
        case detail::kSettingH3Datagram:
            // Boolean settings are 0 or 1 (RFC 9221 §3).
            if (seen_datagram || value > 1)
                return false;
            seen_datagram = true;
            old.datagram = value != 0;
            break;
        case detail::kSettingEnableConnectProtocol:
            if (seen_extended_connect || value > 1)
                return false;
            seen_extended_connect = true;
            old.extended_connect = value != 0;
            break;
        default:
            // An unknown setting might belong to an extension this build no
            // longer understands; the safe answer is to force a full handshake.
            return false;
        }
    }

    if (current.max_field_section_size != 0 &&
        (!seen_max_field || old.max_field_section_size > current.max_field_section_size)) {
        return false;
    }
    if (old.datagram && !current.datagram)
        return false;
    if (old.extended_connect && !current.extended_connect)
        return false;
    if (old.qpack_max_dtable_capacity > current.qpack_max_dtable_capacity)
        return false;
    if (old.qpack_blocked_streams > current.qpack_blocked_streams)
        return false;
    return true;
}

} // namespace simple_http::h3
