#pragma once

// QPACK (RFC 9204) — the encoder and decoder binding to ls-qpack.
//
// QPACK is HPACK with the head-of-line blocking moved somewhere it can be
// bounded: a field section may reference dynamic-table entries the peer has not
// inserted yet, and a decoder is allowed to *block* that one stream rather than
// stall the whole connection. Everything stateful about it — the dynamic table,
// the insert-count arithmetic, the blocked-stream bookkeeping, the Huffman and
// prefix-bit decoding — is ls-qpack's. This file exists only to make its C API
// fit the engine: lifetimes that survive a blocked field section, the
// decoder-stream instructions the library expects its caller to emit, and an
// error mapping onto the HTTP/3 error codes.
//
// The two halves are deliberately asymmetric:
//
//   * The encoder uses the static table only and never inserts. Every field is
//     encoded with LQEF_NO_DYN, which tells the library the dynamic table is
//     neither to be referenced nor modified, so no encoder-stream instruction can
//     ever be produced and there is no insert count to track. That is fully
//     conformant (an implementation may use the static table alone) and it
//     removes the one piece of QPACK state that would otherwise have to be kept
//     in step with the peer's acknowledgements. The encoder stream is still
//     opened, because a peer expects it to exist. (ls-qpack has no
//     "static only" option flag in this version; a zero-capacity dynamic table
//     plus LQEF_NO_DYN is exactly that mode, and is what the library's own
//     documentation describes for an encoder that will not use the table.)
//
//   * The decoder is complete: full dynamic table, blocking, and the three
//     instructions a decoder owes the peer (Section Acknowledgment, Insert Count
//     Increment, Stream Cancellation). A server has no say in whether its clients
//     use the dynamic table, so the decoder has to be able to follow.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lsqpack.h>
// lsqpack.h only forward-declares lsxpack_header; the field layout and the
// accessors live in the companion header.
#include <lsxpack_header.h>

#include "../../proto/headers.h"
#include "h3_frame.h"

namespace simple_http::h3codec {

// One decoded field. A vector of these rather than a Headers: the caller runs the
// RFC 9114 §4.2 validation (lowercase names, no connection-specific fields,
// pseudo-header ordering) before anything is folded into a request, and that
// validation needs to see the fields exactly as they arrived — in order, with
// repeats intact.
using Header = std::pair<std::string, std::string>;

// Names and values longer than this cannot be represented by lsxpack_header at
// all: name_len and val_len are 16-bit, so a longer field would silently truncate
// at the length field. Refusing it turns that into a named protocol error
// instead. It is not a limit on what this server accepts — SETTINGS advertises
// max_header_bytes for that — only on what one QPACK string can hold.
inline constexpr std::size_t kQpackMaxFieldBytes = LSXPACK_MAX_STRLEN;

namespace detail {

// Decoder bookkeeping for one field section.
//
// Heap-allocated and owned by the QpackDecoder rather than kept on the caller's
// stack, because ls-qpack holds the pointer for as long as the block is blocked
// (RFC 9204 §2.1.2) — which is precisely when the caller has stopped running and
// gone off to wait.
struct QpackBlock {
    // Where a block lands once encoder-stream data has freed it. A sink rather
    // than a back-pointer to the decoder, so this struct and the callbacks below
    // need nothing from the class that uses them.
    std::vector<QpackBlock*>* unblocked_sink = nullptr;
    // Opaque to this layer: the engine parks its stream here and gets it back
    // from take_unblocked(), which is what lets a blocked field section be
    // resumed by whoever was waiting for it.
    void* user = nullptr;

    std::uint64_t stream_id = 0;
    std::vector<Header> fields;

    // The decoder writes name and value here, then hands out an lsxpack_header
    // pointing into it. One buffer is enough because ls-qpack fills one field at a
    // time and reports it before preparing the next.
    std::string scratch;
    // The descriptor for that buffer. ls-qpack supplies no storage of its own: it
    // calls back with a null pointer and expects the callback to return one, so
    // this is ours, and it has to outlive process_header().
    lsxpack_header xhdr{};

    // The part of the field section ls-qpack had not consumed when it blocked,
    // kept so that resume() can hand it the remainder.
    const std::uint8_t* cursor = nullptr;
    std::size_t remaining = 0;

    bool unblocked = false;
};

// ls-qpack asks for somewhere to write the next field, sized to `space` octets.
// Refusing (nullptr) is reported by the library as a decode error, which is what
// an over-long field deserves.
//
// `xhdr` is null for a new field and non-null when the library has already
// started writing one and needs the buffer to grow. The growth case is the one
// that matters: the name (or a prefix of the value) is already in the buffer and
// the library will keep writing at an offset into it, so the bytes must survive
// the resize and the descriptor's `buf` must be re-pointed at the new allocation.
// During a decode `val_len` is the *capacity* of the buffer, not the length of a
// value; the library narrows it to the real length before reporting the field.
inline lsxpack_header* qpack_prepare_decode(void* hblock_ctx, lsxpack_header* xhdr, std::size_t space) {
    auto* block = static_cast<QpackBlock*>(hblock_ctx);
    if (space > kQpackMaxFieldBytes) return nullptr;
    if (xhdr == nullptr) {
        block->scratch.assign(space, '\0');
        lsxpack_header_prepare_decode(&block->xhdr, block->scratch.data(), 0, space);
        return &block->xhdr;
    }
    block->scratch.resize(space);
    xhdr->buf = block->scratch.data();
    xhdr->val_len = static_cast<lsxpack_strlen_t>(space);
    return xhdr;
}

inline int qpack_process_header(void* hblock_ctx, lsxpack_header* xhdr) {
    auto* block = static_cast<QpackBlock*>(hblock_ctx);
    const char* name = lsxpack_header_get_name(xhdr);
    // A field with an empty name is not a field, and it is the one case where
    // lsxpack_header_get_name() returns null.
    if (name == nullptr) return -1;
    block->fields.emplace_back(std::string{name, xhdr->name_len},
                               std::string{lsxpack_header_get_value(xhdr), xhdr->val_len});
    return 0;
}

// Called from inside lsqpack_dec_enc_in(), i.e. while the library is still
// walking its own blocked-block list. Resuming the block here would re-enter that
// walk and mutate the list underneath it, so the only thing this does is record
// the block; the decoder hands it back to the engine once the feed has returned.
inline void qpack_block_unblocked(void* hblock_ctx) {
    auto* block = static_cast<QpackBlock*>(hblock_ctx);
    block->unblocked = true;
    if (block->unblocked_sink != nullptr) block->unblocked_sink->push_back(block);
}

inline const lsqpack_dec_hset_if kQpackDecodeInterface{&qpack_block_unblocked, &qpack_prepare_decode,
                                                       &qpack_process_header};

}  // namespace detail

// QPACK response encoder. Static table only; see the file comment.
class QpackEncoder {
  public:
    QpackEncoder() = default;
    ~QpackEncoder();

    QpackEncoder(const QpackEncoder&) = delete;
    QpackEncoder& operator=(const QpackEncoder&) = delete;

    // Initializes with a zero-capacity dynamic table, which is both what a peer
    // assumes before our SETTINGS arrive (RFC 9204 §3.2.3) and all this encoder
    // will ever use — so there is nothing to re-initialize when the peer's
    // SETTINGS do arrive, and a response can be sent without waiting for them.
    // Returns false on a library error.
    bool init();

    // Encodes one complete field section, prefix included, into `out`. `seqno`
    // counts header blocks on the connection from zero; ls-qpack uses it to order
    // its own bookkeeping, and it must never repeat or go backwards. `headers` is
    // encoded exactly as given, pseudo-headers and all.
    bool encode(std::uint64_t stream_id, std::uint64_t seqno, const Headers& headers, Bytes& out);

    // The same, with the :status pseudo-header a response field section must lead
    // with (RFC 9114 §4.3.2) prepended.
    bool encode_response(std::uint64_t stream_id, std::uint64_t seqno, int status, const Headers& headers,
                         Bytes& out);

    bool initialized() const { return m_initialized; }

  private:
    // Encodes one field into the current header block, growing the scratch buffer
    // on LQES_NOBUF_HEAD. Returns false only on a real library error, or for a
    // field that cannot be represented at all — the caller drops that field, since
    // losing one header is a smaller failure than losing the response.
    bool append_field(std::string_view name, std::string_view value, Bytes& body);

    lsqpack_enc m_enc{};
    bool m_initialized = false;
};

// QPACK request decoder. Full dynamic table; see the file comment.
class QpackDecoder {
  public:
    using Block = detail::QpackBlock;

    enum class Status {
        Done,     // the field section is decoded
        Blocked,  // waiting on the peer's encoder stream; resume() it later
        Error,    // the field section is malformed (QPACK_DECOMPRESSION_FAILED)
    };

    QpackDecoder() = default;
    ~QpackDecoder();

    QpackDecoder(const QpackDecoder&) = delete;
    QpackDecoder& operator=(const QpackDecoder&) = delete;

    // `max_table_capacity` and `max_blocked_streams` must be exactly the values
    // this endpoint advertises in SETTINGS. They are the limits ls-qpack holds the
    // peer's encoder to, and a decoder that permitted more than it announced would
    // let a peer overrun the memory the announcement was sized for.
    void init(std::uint32_t max_table_capacity, std::uint32_t max_blocked_streams);

    // Decodes one complete field section.
    //
    // On Done, `out` holds the fields and the Section Acknowledgment owed to the
    // peer has been appended to `decoder_output`. On Blocked, `*block` names the
    // pending field section: the caller must wait until take_unblocked() lists it
    // and then call resume(). On Error the connection is finished —
    // QPACK_DECOMPRESSION_FAILED is a connection error, because the dynamic table
    // state every stream shares can no longer be trusted.
    Status decode(std::uint64_t stream_id, std::string_view section, std::vector<Header>& out,
                  std::string& decoder_output, Block** block);

    // Feeds bytes from the peer's encoder stream. Returns false for
    // QPACK_ENCODER_STREAM_ERROR (also a connection error). May unblock field
    // sections; the caller should follow with take_unblocked().
    bool feed_encoder_stream(std::string_view data);

    // Field sections freed by the last feed_encoder_stream(). Each pointer stays
    // valid until resume() or abandon() is called on it, or this decoder is
    // destroyed; the list itself is emptied by this call.
    std::vector<Block*> take_unblocked();

    // Finishes a field section that has been unblocked. On Done the fields are
    // moved into `out` and `block` is destroyed — it must not be used again; on
    // Error the connection is finished.
    Status resume(Block* block, std::vector<Header>& out, std::string& decoder_output);

    // Releases a field section whose request stream is gone, emitting the Stream
    // Cancellation instruction RFC 9204 §2.2.2.2 asks for — without it the peer
    // keeps a dynamic-table reference open for a stream that will never
    // acknowledge it, and eventually refuses to evict.
    void abandon(Block* block, std::string& decoder_output);

    // Emits any Insert Count Increment the decoder owes (RFC 9204 §4.4.3), which
    // is what lets the peer's encoder release its own history once we have taken
    // entries into the dynamic table.
    void flush_ici(std::string& decoder_output);

  private:
    static void emit_ack(const std::uint8_t* buf, std::size_t len, std::string& decoder_output);
    void forget(Block* block);

    lsqpack_dec m_dec{};
    // Owns every block ls-qpack is still holding. The Block objects live on the
    // heap, so erasing one entry only moves the owning pointers around it — which
    // matters because ls-qpack holds the addresses.
    std::vector<std::unique_ptr<Block>> m_blocks;
    std::vector<Block*> m_unblocked;
    bool m_initialized = false;
};

// --- QpackEncoder ---------------------------------------------------------

inline QpackEncoder::~QpackEncoder() {
    if (m_initialized) lsqpack_enc_cleanup(&m_enc);
}

inline bool QpackEncoder::init() {
    // Both table sizes are zero: the peer's default before its SETTINGS arrive is
    // a zero-capacity dynamic table, and a static-table-only encoder is
    // conformant at any capacity — so one initialization suffices for the life of
    // the connection. At zero capacity no Set Dynamic Table Capacity instruction
    // is generated and the sdtc buffer is not needed, which is why the last two
    // arguments are null.
    if (lsqpack_enc_init(&m_enc, nullptr, /*max_table_size=*/0, /*dyn_table_size=*/0, /*max_risked_streams=*/0,
                         LSQPACK_ENC_OPT_SERVER, nullptr, nullptr) != 0) {
        return false;
    }
    m_initialized = true;
    return true;
}

inline bool QpackEncoder::append_field(std::string_view name, std::string_view value, Bytes& body) {
    if (name.size() > kQpackMaxFieldBytes || value.size() > kQpackMaxFieldBytes) return false;
    // lsxpack_header addresses the name and the value inside one buffer by offset,
    // so the two are laid out back to back here.
    const std::string joined = std::string{name} + std::string{value};
    lsxpack_header xhdr;
    lsxpack_header_set_offset2(&xhdr, joined.data(), 0, name.size(), name.size(), value.size());

    // One field grows at most by the Huffman expansion plus two length prefixes,
    // and the loop below is what makes the exact number irrelevant.
    std::vector<unsigned char> encoded(name.size() + value.size() + 64);
    // LQEF_NO_DYN guarantees nothing is written here; it is a parameter the
    // library insists on, not an output this encoder has any use for.
    std::array<unsigned char, 64> enc_stream{};
    for (;;) {
        std::size_t encoded_sz = encoded.size();
        std::size_t enc_stream_sz = enc_stream.size();
        const lsqpack_enc_status status = lsqpack_enc_encode(&m_enc, enc_stream.data(), &enc_stream_sz,
                                                             encoded.data(), &encoded_sz, &xhdr, LQEF_NO_DYN);
        if (status == LQES_NOBUF_HEAD) {
            encoded.resize(encoded.size() * 2);
            continue;
        }
        if (status != LQES_OK) return false;
        body.append(reinterpret_cast<const char*>(encoded.data()), encoded_sz);
        return true;
    }
}

inline bool QpackEncoder::encode(std::uint64_t stream_id, std::uint64_t seqno, const Headers& headers,
                                 Bytes& out) {
    if (!m_initialized) return false;
    if (lsqpack_enc_start_header(&m_enc, stream_id, static_cast<unsigned>(seqno)) != 0) return false;

    // Whatever happens between here and `end_header`, the block is ended one way
    // or the other. `start_header` sets a state the library only clears in
    // `end_header` or `cancel_header`, and it refuses to start a new block while
    // that state is set — so any early return that skips both does not abandon
    // this one block, it wedges the encoder: every later response fails to encode
    // and the connection resets stream after stream while never closing.
    Bytes body;
    for (const auto& [name, value] : headers.fields()) {
        if (!append_field(name, value, body)) {
            // An application-sized field name or value, not a protocol error:
            // this response is lost, the connection is not.
            (void)lsqpack_enc_cancel_header(&m_enc);
            return false;
        }
    }

    // The prefix (Required Insert Count, Base) is only knowable once the whole
    // section is encoded, but it belongs at the front of it.
    std::array<unsigned char, 32> prefix{};
    const ssize_t prefix_len = lsqpack_enc_end_header(&m_enc, prefix.data(), prefix.size(), nullptr);
    if (prefix_len <= 0) {
        // A failed `end_header` may or may not have cleared the state it was
        // asked to clear; cancelling is a no-op when it did.
        (void)lsqpack_enc_cancel_header(&m_enc);
        return false;
    }
    out.clear();
    out.append(reinterpret_cast<const char*>(prefix.data()), static_cast<std::size_t>(prefix_len));
    out.append(body);
    return true;
}

inline bool QpackEncoder::encode_response(std::uint64_t stream_id, std::uint64_t seqno, int status,
                                          const Headers& headers, Bytes& out) {
    // :status goes through the same path as every other field rather than being
    // pinned to a hard-coded static index: ls-qpack looks the name up in its own
    // static table, so the common statuses already cost a single
    // literal-with-name-reference, without this layer duplicating — and having to
    // stay in step with — that table.
    Headers section;
    section.add_lower(":status", std::to_string(status));
    for (const auto& [name, value] : headers.fields()) section.add_lower(name, value);
    return encode(stream_id, seqno, section, out);
}

// --- QpackDecoder ---------------------------------------------------------

inline QpackDecoder::~QpackDecoder() {
    if (m_initialized) lsqpack_dec_cleanup(&m_dec);
}

inline void QpackDecoder::init(std::uint32_t max_table_capacity, std::uint32_t max_blocked_streams) {
    // No LSQPACK_DEC_OPT_HTTP1X and no hashing: the engine wants the name and
    // value as fields, not a pre-rendered HTTP/1.1 line, and it compares bytes
    // itself.
    lsqpack_dec_init(&m_dec, nullptr, max_table_capacity, max_blocked_streams, &detail::kQpackDecodeInterface,
                     static_cast<lsqpack_dec_opts>(0));
    m_initialized = true;
}

inline void QpackDecoder::emit_ack(const std::uint8_t* buf, std::size_t len, std::string& decoder_output) {
    // A zero length means the library produced no acknowledgement — a block whose
    // Required Insert Count is zero needs none, because it referenced nothing that
    // could be evicted.
    if (len != 0) decoder_output.append(reinterpret_cast<const char*>(buf), len);
}

inline void QpackDecoder::forget(Block* block) {
    for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
        if (it->get() == block) {
            m_blocks.erase(it);
            return;
        }
    }
}

inline QpackDecoder::Status QpackDecoder::decode(std::uint64_t stream_id, std::string_view section,
                                                 std::vector<Header>& out, std::string& decoder_output,
                                                 Block** block) {
    *block = nullptr;
    auto owned = std::make_unique<Block>();
    Block* ctx = owned.get();
    ctx->unblocked_sink = &m_unblocked;
    ctx->stream_id = stream_id;

    const auto* base = reinterpret_cast<const std::uint8_t*>(section.data());
    const std::uint8_t* cursor = base;
    // In/out: the library refuses to write an acknowledgement unless it is told
    // how much room there is, so the capacity goes in and the length comes back.
    std::array<std::uint8_t, LSQPACK_LONGEST_HEADER_ACK> ack{};
    std::size_t ack_sz = ack.size();

    const lsqpack_read_header_status status =
        lsqpack_dec_header_in(&m_dec, ctx, stream_id, section.size(), &cursor, section.size(), ack.data(), &ack_sz);

    switch (status) {
        case LQRHS_DONE:
            emit_ack(ack.data(), ack_sz, decoder_output);
            out = std::move(ctx->fields);
            return Status::Done;
        case LQRHS_BLOCKED:
            ctx->cursor = cursor;
            ctx->remaining = section.size() - static_cast<std::size_t>(cursor - base);
            m_blocks.push_back(std::move(owned));
            *block = ctx;
            return Status::Blocked;
        default:
            // With the whole section in hand, NEED means the section ended in the
            // middle of a field and ERROR means it did not decode at all; §6 makes
            // both QPACK_DECOMPRESSION_FAILED.
            return Status::Error;
    }
}

inline bool QpackDecoder::feed_encoder_stream(std::string_view data) {
    if (data.empty()) return true;
    return lsqpack_dec_enc_in(&m_dec, reinterpret_cast<const std::uint8_t*>(data.data()), data.size()) == 0;
}

inline std::vector<QpackDecoder::Block*> QpackDecoder::take_unblocked() {
    std::vector<Block*> out;
    out.swap(m_unblocked);
    return out;
}

inline QpackDecoder::Status QpackDecoder::resume(Block* block, std::vector<Header>& out,
                                                std::string& decoder_output) {
    const std::uint8_t* cursor = block->cursor;
    std::array<std::uint8_t, LSQPACK_LONGEST_HEADER_ACK> ack{};
    std::size_t ack_sz = ack.size();
    const lsqpack_read_header_status status =
        lsqpack_dec_header_read(&m_dec, block, &cursor, block->remaining, ack.data(), &ack_sz);

    switch (status) {
        case LQRHS_DONE:
            emit_ack(ack.data(), ack_sz, decoder_output);
            // The fields move out before the block dies: ls-qpack has forgotten it,
            // and this decoder's ownership ends with the decode.
            out = std::move(block->fields);
            forget(block);
            return Status::Done;
        case LQRHS_BLOCKED: {
            // §2.1.2 allows a block to be released once; a repeat is therefore a
            // library state this engine does not model, and re-waiting would hang
            // the stream. Treating it as a decode failure at least says so.
            const std::size_t consumed = static_cast<std::size_t>(cursor - block->cursor);
            if (consumed > block->remaining) return Status::Error;
            block->remaining -= consumed;
            block->cursor = cursor;
            return Status::Blocked;
        }
        default:
            forget(block);
            return Status::Error;
    }
}

inline void QpackDecoder::abandon(Block* block, std::string& decoder_output) {
    std::array<std::uint8_t, LSQPACK_LONGEST_CANCEL> buf{};
    const ssize_t written = lsqpack_dec_cancel_stream(&m_dec, block, buf.data(), buf.size());
    if (written > 0) decoder_output.append(reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(written));
    forget(block);
}

inline void QpackDecoder::flush_ici(std::string& decoder_output) {
    while (lsqpack_dec_ici_pending(&m_dec)) {
        std::array<std::uint8_t, LSQPACK_LONGEST_ICI> buf{};
        const ssize_t written = lsqpack_dec_write_ici(&m_dec, buf.data(), buf.size());
        // Zero means the increment no longer applies and a negative value is a
        // buffer error that cannot happen at this size; either way there is
        // nothing to write, and looping again would spin.
        if (written <= 0) return;
        decoder_output.append(reinterpret_cast<const char*>(buf.data()), static_cast<std::size_t>(written));
    }
}

// --- decoder stream (RFC 9204 §4.4) ---------------------------------------
//
// The *peer's* decoder stream carries instructions about the field sections
// this endpoint's encoder produced: Section Acknowledgment, Stream Cancellation
// and Insert Count Increment. They exist so the encoder can release dynamic
// table state, which is why an encoder that never inserts (this one) has nothing
// to do with them — but validating them is not optional, and the two rules that
// matter are both "the peer is confused about what we sent":
//
//   * an Insert Count Increment of 0 acknowledges nothing while looking like
//     progress, which §4.4.3 makes a decoder stream error;
//   * a Section Acknowledgment or Stream Cancellation naming a field section
//     this encoder never wrote is the same (§4.4.1, §4.4.2). With a
//     static-table-only encoder *every* such instruction names one, so any of
//     them is an error — not a harmless extra.

enum class DecoderInstruction : std::uint8_t {
    SectionAcknowledgment,
    StreamCancellation,
    InsertCountIncrement,
};

// Reads the integer form RFC 7541 §5.1 gives every QPACK instruction: a value in
// the low bits of the first octet, continued in 7-bit groups while the high bit
// is set. Returns false when `data` holds only part of one — instructions arrive
// on a byte stream, and a split one is not an error, only a reason to read more.
inline bool read_prefixed_int(std::string_view data, unsigned prefix_bits, std::uint64_t& value,
                              std::size_t& consumed) {
    if (data.empty()) return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::uint64_t mask = (std::uint64_t{1} << prefix_bits) - 1;
    std::uint64_t v = bytes[0] & mask;
    if (v < mask) {
        value = v;
        consumed = 1;
        return true;
    }
    std::size_t i = 1;
    unsigned shift = 0;
    for (;;) {
        if (i >= data.size()) return false;
        const std::uint8_t byte = bytes[i++];
        v += static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        // Far past the 62 bits a QPACK integer can hold, so the peer is not
        // sending an integer at all.
        if (shift > 56) return false;
    }
    value = v;
    consumed = i;
    return true;
}

// One instruction, or false when the buffer does not yet hold a whole one.
inline bool read_decoder_instruction(std::string_view data, DecoderInstruction& kind, std::uint64_t& value,
                                     std::size_t& consumed) {
    if (data.empty()) return false;
    const auto first = static_cast<std::uint8_t>(data[0]);
    if ((first & 0x80) != 0) {
        kind = DecoderInstruction::SectionAcknowledgment;
        return read_prefixed_int(data, 7, value, consumed);
    }
    if ((first & 0x40) != 0) {
        kind = DecoderInstruction::StreamCancellation;
        return read_prefixed_int(data, 6, value, consumed);
    }
    kind = DecoderInstruction::InsertCountIncrement;
    return read_prefixed_int(data, 6, value, consumed);
}

}  // namespace simple_http::h3codec
