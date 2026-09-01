// Module wire_protocol.d - Compact Field Encoding & Wire Framing for Low-Latency Transport
module wire_protocol;

import core.stdc.string : memcpy, memset;

@nogc:
nothrow:


// --- Event Type Constants ---
enum ushort TYPE_TRADE       = 1;
enum ushort TYPE_BBO         = 2;
enum ushort TYPE_ORDERBOOK   = 3;
enum ushort TYPE_FEC_PARITY  = 100;
enum ushort TYPE_NAK_REQ     = 101;

// --- Harness C++ Struct Alignment & Layouts ---
align(1) struct HarnessHeader {
    ulong  seq_id;      // 8 bytes
    ulong  send_ts_ns;  // 8 bytes
    ushort type;        // 2 bytes
    ushort version_;    // 2 bytes
    uint   body_len;    // 4 bytes
}
static assert(HarnessHeader.sizeof == 24);

enum uint MAX_SYMBOL_LEN   = 16;
enum uint MAX_VENUE_LEN    = 16;
enum uint MAX_CURRENCY_LEN = 8;
enum uint BOOK_DEPTH       = 5;

align(64) struct HarnessTrade {
    HarnessHeader header;                // 24 bytes
    char[MAX_SYMBOL_LEN] symbol;         // 16 bytes
    char[MAX_VENUE_LEN] venue;           // 16 bytes
    char[MAX_CURRENCY_LEN] base_currency;// 8 bytes
    char[MAX_CURRENCY_LEN] quote_currency;// 8 bytes
    ulong trade_id;                      // 8 bytes
    ulong buyer_order_id;                // 8 bytes
    ulong seller_order_id;               // 8 bytes
    ulong exchange_ts_ns;                // 8 bytes
    ulong match_engine_ts_ns;            // 8 bytes
    double price;                        // 8 bytes
    double quantity;                     // 8 bytes
    double notional;                     // 8 bytes
    long price_ticks;                    // 8 bytes
    long quantity_lots;                  // 8 bytes
    uint tick_direction;                 // 4 bytes
    ubyte aggressor_side;                // 1 byte
    ubyte is_block_trade;                // 1 byte
    ubyte is_rpi;                        // 1 byte
    ubyte is_liquidation;                // 1 byte
    ubyte flags;                         // 1 byte
    ubyte[19] reserved;                  // 19 bytes
}
static assert(HarnessTrade.sizeof == 192);

align(64) struct HarnessBbo {
    HarnessHeader header;                // 24 bytes
    char[MAX_SYMBOL_LEN] symbol;         // 16 bytes
    char[MAX_VENUE_LEN] venue;           // 16 bytes
    ulong update_id;                     // 8 bytes
    ulong exchange_ts_ns;                // 8 bytes
    ulong match_engine_ts_ns;            // 8 bytes
    double bid_price;                    // 8 bytes
    double bid_size;                     // 8 bytes
    double ask_price;                    // 8 bytes
    double ask_size;                     // 8 bytes
    long bid_price_ticks;                // 8 bytes
    long ask_price_ticks;                // 8 bytes
    long bid_size_lots;                  // 8 bytes
    long ask_size_lots;                  // 8 bytes
    uint bid_order_count;                // 4 bytes
    uint ask_order_count;                // 4 bytes
    ubyte flags;                         // 1 byte
    ubyte[23] reserved;                  // 23 bytes
}
static assert(HarnessBbo.sizeof == 192);

align(1) struct HarnessBookLevel {
    double price;        // 8 bytes
    double size;         // 8 bytes
    long price_ticks;    // 8 bytes
    long size_lots;      // 8 bytes
    uint order_count;    // 4 bytes
    uint reserved;       // 4 bytes
}
static assert(HarnessBookLevel.sizeof == 40);

align(64) struct HarnessOrderBook {
    HarnessHeader header;                // 24 bytes
    char[MAX_SYMBOL_LEN] symbol;         // 16 bytes
    char[MAX_VENUE_LEN] venue;           // 16 bytes
    ulong update_id;                     // 8 bytes
    ulong prev_update_id;                // 8 bytes
    ulong exchange_ts_ns;                // 8 bytes
    ulong match_engine_ts_ns;            // 8 bytes
    HarnessBookLevel[BOOK_DEPTH] bids;   // 200 bytes
    HarnessBookLevel[BOOK_DEPTH] asks;   // 200 bytes
    uint checksum;                       // 4 bytes
    ubyte is_snapshot;                   // 1 byte
    ubyte flags;                         // 1 byte
    ubyte[26] reserved;                  // 26 bytes
}
static assert(HarnessOrderBook.sizeof == 576);

// --- Wire Format (Stateless Compact Field Encoding) ---
align(1) struct CompactWireHeader {
    align(1):
    ulong  seq_id;      // 8 bytes
    ulong  send_ts_ns;  // 8 bytes
    ushort type;        // 2 bytes
    ushort version_;    // 2 bytes
}
static assert(CompactWireHeader.sizeof == 20);

align(1) struct CompactTradeWire {
    align(1):
    CompactWireHeader header;         // 20 bytes
    ulong             trade_id;       // 8 bytes
    ulong             exchange_ts_ns; // 8 bytes
    int               price_ticks;    // 4 bytes
    int               quantity_lots;  // 4 bytes
    ubyte             aggressor_side; // 1 byte
    ubyte             flags;          // 1 byte
    ushort            venue_id;       // 2 bytes
}
static assert(CompactTradeWire.sizeof == 48);

align(1) struct CompactBboWire {
    align(1):
    CompactWireHeader header;          // 20 bytes
    ulong             update_id;       // 8 bytes
    ulong             exchange_ts_ns;  // 8 bytes
    int               bid_price_ticks; // 4 bytes
    int               ask_price_ticks; // 4 bytes
    int               bid_size_lots;   // 4 bytes
    int               ask_size_lots;   // 4 bytes
}
static assert(CompactBboWire.sizeof == 52);

align(1) struct CompactBookLevelWire {
    align(1):
    int price_ticks; // 4 bytes
    int size_lots;   // 4 bytes
}
static assert(CompactBookLevelWire.sizeof == 8);

align(1) struct CompactOrderBookWire {
    align(1):
    CompactWireHeader          header;         // 20 bytes
    ulong                      update_id;      // 8 bytes
    ulong                      exchange_ts_ns; // 8 bytes
    CompactBookLevelWire[5]    bids;           // 40 bytes
    CompactBookLevelWire[5]    asks;           // 40 bytes
    uint                       checksum;       // 4 bytes
    ubyte                      flags;          // 1 byte
    ubyte                      reserved_pad;   // 1 byte
}
static assert(CompactOrderBookWire.sizeof == 122);

// --- Forward Error Correction & NAK Packet Structures ---
align(1) struct FecParityWire {
    align(1):
    CompactWireHeader header;                 // 20 bytes
    ulong             group_id;               // 8 bytes
    ulong             base_seq_id;            // 8 bytes
    ushort            packet_count;           // 2 bytes
    ushort            mask_bitmap;            // 2 bytes
    ubyte[256]        xor_payload;            // 256 bytes
}

align(1) struct NakReqWire {
    align(1):
    CompactWireHeader header;                 // 20 bytes
    ulong             start_seq;              // 8 bytes
    ulong             end_seq;                // 8 bytes
    uint              receiver_id;            // 4 bytes
}

// --- Encoding / Decoding Implementations ---
uint encodeTrade(const HarnessTrade* raw, ubyte* out_buf) @nogc nothrow {
    auto wire = cast(CompactTradeWire*)out_buf;
    wire.header.seq_id      = raw.header.seq_id;
    wire.header.send_ts_ns  = raw.header.send_ts_ns;
    wire.header.type        = raw.header.type;
    wire.header.version_    = raw.header.version_;

    wire.trade_id       = raw.trade_id;
    wire.exchange_ts_ns = raw.exchange_ts_ns;
    wire.price_ticks    = cast(int)raw.price_ticks;
    wire.quantity_lots  = cast(int)raw.quantity_lots;
    wire.aggressor_side = raw.aggressor_side;
    wire.flags          = raw.flags;
    wire.venue_id       = 1;

    return CompactTradeWire.sizeof;
}

uint decodeTrade(const ubyte* in_buf, HarnessTrade* out_raw) @nogc nothrow {
    auto wire = cast(const CompactTradeWire*)in_buf;
    memset(out_raw, 0, HarnessTrade.sizeof);

    out_raw.header.seq_id     = wire.header.seq_id;
    out_raw.header.send_ts_ns = wire.header.send_ts_ns;
    out_raw.header.type       = wire.header.type;
    out_raw.header.version_   = wire.header.version_;
    out_raw.header.body_len   = HarnessTrade.sizeof;

    out_raw.trade_id          = wire.trade_id;
    out_raw.exchange_ts_ns    = wire.exchange_ts_ns;
    out_raw.price_ticks       = wire.price_ticks;
    out_raw.quantity_lots     = wire.quantity_lots;
    out_raw.aggressor_side    = wire.aggressor_side;
    out_raw.flags             = wire.flags;
    out_raw.price             = cast(double)wire.price_ticks * 0.01;
    out_raw.quantity          = cast(double)wire.quantity_lots * 0.001;

    return HarnessTrade.sizeof;
}

uint encodeBbo(const HarnessBbo* raw, ubyte* out_buf) @nogc nothrow {
    auto wire = cast(CompactBboWire*)out_buf;
    wire.header.seq_id      = raw.header.seq_id;
    wire.header.send_ts_ns  = raw.header.send_ts_ns;
    wire.header.type        = raw.header.type;
    wire.header.version_    = raw.header.version_;

    wire.update_id       = raw.update_id;
    wire.exchange_ts_ns  = raw.exchange_ts_ns;
    wire.bid_price_ticks = cast(int)raw.bid_price_ticks;
    wire.ask_price_ticks = cast(int)raw.ask_price_ticks;
    wire.bid_size_lots   = cast(int)raw.bid_size_lots;
    wire.ask_size_lots   = cast(int)raw.ask_size_lots;

    return CompactBboWire.sizeof;
}

uint decodeBbo(const ubyte* in_buf, HarnessBbo* out_raw) @nogc nothrow {
    auto wire = cast(const CompactBboWire*)in_buf;
    memset(out_raw, 0, HarnessBbo.sizeof);

    out_raw.header.seq_id     = wire.header.seq_id;
    out_raw.header.send_ts_ns = wire.header.send_ts_ns;
    out_raw.header.type       = wire.header.type;
    out_raw.header.version_   = wire.header.version_;
    out_raw.header.body_len   = HarnessBbo.sizeof;

    out_raw.update_id       = wire.update_id;
    out_raw.exchange_ts_ns  = wire.exchange_ts_ns;
    out_raw.bid_price_ticks = wire.bid_price_ticks;
    out_raw.ask_price_ticks = wire.ask_price_ticks;
    out_raw.bid_size_lots   = wire.bid_size_lots;
    out_raw.ask_size_lots   = wire.ask_size_lots;
    out_raw.bid_price       = cast(double)wire.bid_price_ticks * 0.01;
    out_raw.ask_price       = cast(double)wire.ask_price_ticks * 0.01;
    out_raw.bid_size        = cast(double)wire.bid_size_lots * 0.001;
    out_raw.ask_size        = cast(double)wire.ask_size_lots * 0.001;

    return HarnessBbo.sizeof;
}

uint encodeBook(const HarnessOrderBook* raw, ubyte* out_buf) @nogc nothrow {
    auto wire = cast(CompactOrderBookWire*)out_buf;
    wire.header.seq_id      = raw.header.seq_id;
    wire.header.send_ts_ns  = raw.header.send_ts_ns;
    wire.header.type        = raw.header.type;
    wire.header.version_    = raw.header.version_;

    wire.update_id       = raw.update_id;
    wire.exchange_ts_ns  = raw.exchange_ts_ns;
    wire.checksum        = raw.checksum;
    wire.flags           = raw.flags;

    for (int i = 0; i < 5; ++i) {
        wire.bids[i].price_ticks = cast(int)raw.bids[i].price_ticks;
        wire.bids[i].size_lots  = cast(int)raw.bids[i].size_lots;
        wire.asks[i].price_ticks = cast(int)raw.asks[i].price_ticks;
        wire.asks[i].size_lots  = cast(int)raw.asks[i].size_lots;
    }

    return CompactOrderBookWire.sizeof;
}

uint decodeBook(const ubyte* in_buf, HarnessOrderBook* out_raw) @nogc nothrow {
    auto wire = cast(const CompactOrderBookWire*)in_buf;
    memset(out_raw, 0, HarnessOrderBook.sizeof);

    out_raw.header.seq_id     = wire.header.seq_id;
    out_raw.header.send_ts_ns = wire.header.send_ts_ns;
    out_raw.header.type       = wire.header.type;
    out_raw.header.version_   = wire.header.version_;
    out_raw.header.body_len   = HarnessOrderBook.sizeof;

    out_raw.update_id      = wire.update_id;
    out_raw.exchange_ts_ns = wire.exchange_ts_ns;
    out_raw.checksum       = wire.checksum;
    out_raw.flags          = wire.flags;

    for (int i = 0; i < 5; ++i) {
        out_raw.bids[i].price_ticks = wire.bids[i].price_ticks;
        out_raw.bids[i].size_lots   = wire.bids[i].size_lots;
        out_raw.bids[i].price       = cast(double)wire.bids[i].price_ticks * 0.01;
        out_raw.bids[i].size        = cast(double)wire.bids[i].size_lots * 0.001;

        out_raw.asks[i].price_ticks = wire.asks[i].price_ticks;
        out_raw.asks[i].size_lots   = wire.asks[i].size_lots;
        out_raw.asks[i].price       = cast(double)wire.asks[i].price_ticks * 0.01;
        out_raw.asks[i].size        = cast(double)wire.asks[i].size_lots * 0.001;
    }

    return HarnessOrderBook.sizeof;
}
