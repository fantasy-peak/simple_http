#pragma once

// `QuicConnectionConfig` → ngtcp2 settings and transport parameters.
//
// The struct is the user-visible knob set (`ServerConfig::quic_options`), and it
// predates ngtcp2; this file is the translation, and the place where the two
// vocabularies disagree is written down rather than discovered later.
//
// Three of the knobs have no ngtcp2 counterpart at all. They are kept in the
// struct — deleting a field is a compile error for downstream code, keeping an
// inert one is only a semantic downgrade — and marked here so that nobody
// spends an afternoon looking for where they went.

#include <cstddef>
#include <cstdint>

#include <ngtcp2/ngtcp2.h>

namespace simple_http::quic {

// Tunables for a connection, all advertised to the peer in transport
// parameters except the purely local ones.
//
// Four fields no longer reach the wire — ngtcp2 has no counterpart for them.
// They are kept rather than deleted because removing a field breaks downstream
// code at compile time, while an inert field only misleads. The reason for each
// omission is written down next to its mapping, at the bottom of this file.
struct QuicConnectionConfig {
    std::uint64_t max_idle_timeout_ms{30000};
    std::uint64_t max_udp_payload_size{1472};
    std::uint64_t initial_max_data{1u << 20};
    std::uint64_t initial_max_stream_data_bidi_local{256u << 10};
    std::uint64_t initial_max_stream_data_bidi_remote{256u << 10};
    std::uint64_t initial_max_stream_data_uni{256u << 10};
    std::uint64_t initial_max_streams_bidi{100};
    std::uint64_t initial_max_streams_uni{100};
    std::uint64_t active_connection_id_limit{4};
    std::uint64_t ack_delay_exponent{3};
    std::uint64_t max_ack_delay_ms{25};
    // Minimum number of received ACK-eliciting packets before ngtcp2 sends an
    // immediate acknowledgement; below it the ACK waits for the ack timer.
    // ngtcp2's own default is 2, but 1 is the shipped default here: delay
    // inflates the remote's observed RTT (measured on this host: smoothed RTT
    // 35-93 ms instead of 0.4 ms), which stalls the peer's cwnd growth as soon
    // as requests carry payload (POST 512B: 10.3k → 17.5k req/s at ack_thresh
    // 2 → 1). The cost is one extra ~31 B ACK packet per data packet, which is
    // nothing versus an RTT sample that trails the real RTT by an order of
    // magnitude, and ACKing early is always RFC-compliant (≤ the advertised
    // max_ack_delay).
    std::uint64_t ack_thresh{1};

    std::size_t connection_id_length{8};
    // Inert under ngtcp2 (see ngtcp2_config.h): CRYPTO_BUFFER_EXCEEDED is
    // ngtcp2's to raise.
    std::size_t max_crypto_buffer{64 * 1024};
    // Inert under ngtcp2: the stream table is ngtcp2's now, and
    // `initial_max_streams_*` are the only limits that exist.
    std::size_t max_streams{512};
    bool disable_active_migration{false};
    bool enable_0rtt{false};
};

// How ngtcp2 is told about one connection.
struct Ngtcp2Params {
    ngtcp2_settings settings{};
    ngtcp2_transport_params transport{};
};

// Fill `out` from `cfg`. `ts` is the connection's initial timestamp; ngtcp2
// needs one to start its loss-detection clock, and a connection that begins at
// timestamp zero would compute an RTT against the epoch.
//
// `original_dcid` / `retry_scid` are set by the endpoint, which is the only
// place that knows whether a Retry was issued (RFC 9000 §7.3 requires the
// server to echo both back, and §7.4.1 lets the client detect a forgery when
// they disagree).
inline Ngtcp2Params make_ngtcp2_params(const QuicConnectionConfig& cfg, ngtcp2_tstamp ts) {
    Ngtcp2Params out;

    ngtcp2_settings_default(&out.settings);
    out.settings.initial_ts = ts;
    out.settings.max_tx_udp_payload_size = cfg.max_udp_payload_size;
    out.settings.ack_thresh = static_cast<std::size_t>(cfg.ack_thresh);

    // Today's stack treats `max_udp_payload_size` as a hard ceiling, and so does
    // this: ngtcp2 would otherwise probe for a larger path MTU and start sending
    // bigger datagrams, which is a real behaviour change (and one that breaks on
    // a tunnel that drops ICMP). Turning PMTUD off keeps the wire behaviour
    // identical to the hand-written stack's.
    out.settings.no_pmtud = 1;
    // The same flag the reference server sets when it pins the payload size:
    // without it ngtcp2 shapes outgoing datagrams to a smaller size while it is
    // still probing, which would make `max_udp_payload_size` advisory.
    out.settings.no_tx_udp_payload_size_shaping = 1;

    // `available_versions` is not set here: the version list belongs to the
    // listener (`QuicEndpointConfig::supported_versions`), not to one
    // connection, and the connection sets it after this call from the endpoint's
    // configuration.

    ngtcp2_transport_params_default(&out.transport);
    out.transport.initial_max_stream_data_bidi_local = cfg.initial_max_stream_data_bidi_local;
    out.transport.initial_max_stream_data_bidi_remote = cfg.initial_max_stream_data_bidi_remote;
    out.transport.initial_max_stream_data_uni = cfg.initial_max_stream_data_uni;
    out.transport.initial_max_data = cfg.initial_max_data;
    out.transport.initial_max_streams_bidi = cfg.initial_max_streams_bidi;
    out.transport.initial_max_streams_uni = cfg.initial_max_streams_uni;
    out.transport.max_idle_timeout = cfg.max_idle_timeout_ms * NGTCP2_MILLISECONDS;
    out.transport.max_udp_payload_size = cfg.max_udp_payload_size;
    out.transport.active_connection_id_limit = cfg.active_connection_id_limit;
    out.transport.ack_delay_exponent = cfg.ack_delay_exponent;
    out.transport.max_ack_delay = cfg.max_ack_delay_ms * NGTCP2_MILLISECONDS;
    out.transport.disable_active_migration = cfg.disable_active_migration ? 1 : 0;
    // The server always has a token to hand out: stateless reset is how a
    // connection that has lost its state answers a peer that still believes in
    // it, and the token is what makes the answer unforgeable.
    out.transport.stateless_reset_token_present = 1;
    out.transport.grease_quic_bit = 1;

    // No ngtcp2 counterpart:
    //
    //   * `max_crypto_buffer` — ngtcp2 bounds the crypto stream itself and
    //     raises CRYPTO_BUFFER_EXCEEDED on its own terms.
    //   * `max_streams`       — this was a backstop behind MAX_STREAMS, guarding
    //     our stream table. The table belongs to ngtcp2 now, and
    //     `initial_max_streams_bidi`/`uni` are the only limits that exist.
    //
    // Both fields are still read by callers and still mean what they say to
    // anyone reading the struct; they just no longer reach the wire.
    //
    // `enable_0rtt` is a real ngtcp2 concept but needs a session cache the
    // server has nowhere to keep, so it stays off — which is what the default
    // `false` already says.

    return out;
}

}  // namespace simple_http::quic
