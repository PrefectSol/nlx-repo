#ifndef NLX_CORE_MARKET_H
#define NLX_CORE_MARKET_H

#include <cstddef>
#include <cstdint>

namespace nlx
{

struct Instrument
{
    static constexpr size_t kIdCapacity = 32;

    char id[kIdCapacity];
    char family[kIdCapacity];

    int64_t code;

    double contract_value;
    double contract_multiplier;
    double lot_size;
    double min_size;
    double tick_size;
    double max_limit_size;
    double max_leverage;

    int fee_group;
    bool accepted;
    bool restricted;
};

struct Candle
{
    int64_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

struct Volume
{
    int64_t timestamp;
    double volume;
    bool confirmed;
};

struct Balance
{
    double equity;
    double available;
    double isolated_pnl;
};

struct Fee
{
    int group;
    double maker;
    double taker;
};

struct Tier
{
    double min_size;
    double max_size;
    double max_leverage;
};

struct Trade
{
    int64_t timestamp;
    double price;
    double size;
};

struct Level
{
    double price;
    double size;
};

struct Bar
{
    int64_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;
    double bid;
    double ask;
};

enum class MarketUpdateType : uint8_t
{
    Trade,
    Book,
    Clock,
    Failure
};

struct MarketUpdate
{
    MarketUpdateType type;
    size_t slot;
    int64_t timestamp;
    double price;
    double size;
};

enum class AccountUpdateType : uint8_t
{
    Balance,
    Positions,
    Orders,
    Reply
};

struct Position
{
    char instrument[Instrument::kIdCapacity];
    char side[8];

    double size;
    double price;
    double liquidation;
    double margin;
    double pnl;
};

struct Order
{
    char instrument[Instrument::kIdCapacity];
    char id[Instrument::kIdCapacity];
    char client[Instrument::kIdCapacity];
    char side[8];
    char position[8];
    char state[24];
    char type[24];

    double size;
    double filled;
    double price;
    double fee;
    double pnl;

    int64_t updated;
};

struct OrderReply
{
    char operation[24];
    char client[Instrument::kIdCapacity];
    char id[Instrument::kIdCapacity];
    char message[160];

    int code;
    int status;

    int64_t received;
    int64_t sent;
};

enum class PositionSide : uint8_t
{
    Long,
    Short
};

enum class OrderType : uint8_t
{
    Market,
    Limit,
    PostOnly,
    Ioc
};

struct OrderTicket
{
    char client[Instrument::kIdCapacity];
    char size[24];
    char price[24];

    int64_t instrument;

    PositionSide position;
    OrderType type;
    bool buy;
};

} // namespace nlx
#endif // !NLX_CORE_MARKET_H
