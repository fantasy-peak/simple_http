// The SETTINGS snapshot carried in a TLS session ticket decides whether an
// early request may be replayed. These tests pin the compatibility matrix and
// the wire encoding so a future SETTINGS change cannot silently accept a 0-RTT
// request encoded against limits this build no longer advertises.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#include <simple_http/engine/h3/h3_settings.h>

#ifdef SIMPLE_HTTP_ENABLE_HTTP3

using namespace simple_http;

namespace {

std::string make_ticket(std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> pairs,
                        std::uint64_t frame_type = 0x04) {
    std::string payload;
    for (const auto &[id, value] : pairs) {
        h3::detail::put_uvarint(payload, id);
        h3::detail::put_uvarint(payload, value);
    }
    std::string out{h3::kTicketSettingsPrefix};
    h3::detail::put_uvarint(out, frame_type);
    h3::detail::put_uvarint(out, payload.size());
    out += payload;
    return out;
}

h3::Settings snapshot(std::uint64_t max_field = 65536, std::uint64_t qpack_capacity = 4096,
                      std::uint64_t qpack_blocked = 16, bool datagram = false, bool extended_connect = false) {
    h3::Settings settings;
    settings.max_field_section_size = max_field;
    settings.qpack_max_dtable_capacity = qpack_capacity;
    settings.qpack_blocked_streams = qpack_blocked;
    settings.datagram = datagram;
    settings.extended_connect = extended_connect;
    return settings;
}

} // namespace

TEST_CASE("h3 settings: the encoded snapshot is compatible with itself", "[h3][0rtt]") {
    const h3::Settings current = snapshot();
    const std::string ticket = h3::encode_ticket_settings(current);
    CHECK(h3::ticket_settings_compatible_for_0rtt(ticket, current));
}

TEST_CASE("h3 settings: a smaller current field section rejects the ticket", "[h3][0rtt]") {
    const std::string ticket = h3::encode_ticket_settings(snapshot(65536));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(ticket, snapshot(32768)));
    CHECK(h3::ticket_settings_compatible_for_0rtt(ticket, snapshot(131072)));
}

TEST_CASE("h3 settings: QPACK limits may not shrink", "[h3][0rtt]") {
    const std::string capacity = h3::encode_ticket_settings(snapshot(65536, 8192, 16));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(capacity, snapshot(65536, 4096, 16)));

    const std::string blocked = h3::encode_ticket_settings(snapshot(65536, 4096, 32));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(blocked, snapshot(65536, 4096, 16)));

    const std::string smaller = h3::encode_ticket_settings(snapshot(65536, 1024, 4));
    CHECK(h3::ticket_settings_compatible_for_0rtt(smaller, snapshot(65536, 4096, 16)));
}

TEST_CASE("h3 settings: datagram and extended CONNECT may not disappear", "[h3][0rtt]") {
    const std::string datagram = h3::encode_ticket_settings(snapshot(65536, 4096, 16, true, false));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(datagram, snapshot()));
    CHECK(h3::ticket_settings_compatible_for_0rtt(datagram, snapshot(65536, 4096, 16, true, false)));

    const std::string connect = h3::encode_ticket_settings(snapshot(65536, 4096, 16, false, true));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(connect, snapshot()));
    CHECK(h3::ticket_settings_compatible_for_0rtt(connect, snapshot(65536, 4096, 16, false, true)));
}

TEST_CASE("h3 settings: malformed or unknown snapshots reject 0-RTT", "[h3][0rtt]") {
    const h3::Settings current = snapshot();
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt("", current));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt("not a ticket", current));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(make_ticket({{0x06, 65536}}, 0x05), current));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(make_ticket({{0x42, 1}}), current));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(make_ticket({{0x06, 1}, {0x06, 1}}), current));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(make_ticket({{0x01, 4096}, {0x07, 16}}), current));
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(make_ticket({{0x33, 2}}), current));

    // A frame whose length does not consume the rest of the payload.
    std::string bad = make_ticket({{0x06, 65536}});
    bad.push_back('x');
    CHECK_FALSE(h3::ticket_settings_compatible_for_0rtt(bad, current));
}

#endif
