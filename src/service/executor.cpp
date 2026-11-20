#include "service/executor.h"
#include "core/string.h"
#include "exchange/okx_account_streamer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>

namespace nlx::service
{

namespace
{

constexpr std::string_view side_name(PositionSide side) noexcept
{
    return side == PositionSide::Long ? "long" : "short";
}

constexpr size_t side_index(PositionSide side) noexcept
{
    return side == PositionSide::Long ? 0 : 1;
}

int decimals(double step) noexcept
{
    double scaled = step;
    for (int digits = 0; digits <= 12; ++digits)
    {
        if (std::abs(scaled - std::round(scaled)) < 1e-6 * std::max(1.0, scaled))
        {
            return digits;
        }

        scaled *= 10.0;
    }

    return 12;
}

template<size_t N>
bool format_step(double value, double step, bool up, char (&out)[N]) noexcept
{
    if (step <= 0.0 || value <= 0.0)
    {
        return false;
    }

    const double ratio = value / step;
    const double units = up ? std::ceil(ratio - 1e-9) : std::floor(ratio + 1e-9);

    if (units < 1.0)
    {
        return false;
    }

    const auto written = std::format_to_n(out, N - 1, "{:.{}f}", units * step, decimals(step));
    out[written.size] = '\0';

    return static_cast<size_t>(written.size) < N - 1;
}

bool terminal(std::string_view state) noexcept
{
    return state == "filled" || state == "canceled" || state == "mmp_canceled";
}

} // namespace

template<typename Streamer>
Executor<Streamer>::Executor(Streamer &streamer, logging::LoggerNode &log, uint32_t tag) noexcept :
    m_streamer(streamer),
    m_log(log),
    m_tickets(),
    m_holdings(),
    m_known(),
    m_seats(),
    m_events(),
    m_known_size(0),
    m_known_next(0),
    m_events_head(0),
    m_events_tail(0),
    m_tag(tag),
    m_sequence(0),
    m_draining(false),
    m_order_trip(),
    m_cancel_trip(),
    m_gateway(),
    m_fill(),
    m_push(),
    m_rejects(0)
{

}

template<typename Streamer>
void Executor<Streamer>::command(const ExecutorCommand &command, nanoseconds now) noexcept
{
    if (command.slot >= kSlotsCapacity) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx executor command rejected: slot = {} is out of range", command.slot);
        return;
    }

    if (command.type == ExecutorCommandType::Open)
    {
        open(command, now);
        return;
    }

    close(command, now);
}

template<typename Streamer>
void Executor<Streamer>::open(const ExecutorCommand &command, nanoseconds now) noexcept
{
    Seat &seat = m_seats[command.slot];
    const std::string_view instrument(command.instrument);

    const bool foreign = seat.bound && seat.episode != command.episode;
    const bool busy = seat.bound && (seat.closing || seat.releasing);

    if (foreign || busy || m_draining) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx executor open rejected: slot = {}, episode = {}, foreign = {}, closing = {}, draining = {}",
                                              command.slot, command.episode, foreign, busy, m_draining);
        emit(AccountEventType::Rejected, command.slot, command.episode, 0.0, 0.0, -1, command.side, false);

        return;
    }

    if (!seat.bound)
    {
        seat = Seat{};
        nlx::detail::str_assign(seat.instrument, instrument);
        seat.episode = command.episode;
        seat.code = command.code;
        seat.lot = command.lot;
        seat.activity = now;
        seat.bound = true;
    }

    if (find_known(instrument) == nullptr)
    {
        Known &known = m_known[m_known_next];
        nlx::detail::str_assign(known.instrument, instrument);
        known.code = command.code;
        known.lot = command.lot;

        m_known_next = (m_known_next + 1) % kKnownCapacity;
        m_known_size = std::min(m_known_size + 1, kKnownCapacity);
    }

    const bool buy = command.side == PositionSide::Long;

    OrderTicket ticket{};
    ticket.instrument = command.code;
    ticket.position = command.side;
    ticket.type = command.order;
    ticket.buy = buy;

    bool valid = command.code != 0 && format_step(command.size, command.lot, false, ticket.size);
    valid = valid && (command.order == OrderType::Market || format_step(command.price, command.tick, !buy, ticket.price));

    Ticket *const target = valid ? allocate() : nullptr;
    if (target == nullptr) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx executor open rejected: slot = {}, instrument = {}, code = {}, size = {}, price = {}, valid = {}",
                                            command.slot, instrument, command.code, command.size, command.price, valid);
        emit(AccountEventType::Rejected, command.slot, command.episode, 0.0, 0.0, -2, command.side, false);

        return;
    }

    const auto written = std::format_to_n(target->client, sizeof(target->client) - 1, "n{}e{}o{}", m_tag, command.episode, ++m_sequence);
    target->client[written.size] = '\0';

    std::copy_n(target->client, sizeof(ticket.client), ticket.client);

    nlx::detail::str_assign(target->instrument, instrument);
    target->code = command.code;
    target->slot = command.slot;
    target->episode = command.episode;
    target->size = command.size;
    target->sent = now;
    target->side = command.side;
    target->closing = false;
    target->used = true;

    if (!m_streamer.place(ticket)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx executor order not sent: slot = {}, client = {}", command.slot, std::string_view(target->client));

        *target = Ticket{};
        emit(AccountEventType::Rejected, command.slot, command.episode, 0.0, 0.0, -3, command.side, false);

        return;
    }

    seat.activity = now;

    m_log.log("okx executor order sent: slot = {}, client = {}, instrument = {}, side = {}, size = {}, price = {}",
              command.slot, std::string_view(ticket.client), instrument, side_name(command.side), std::string_view(ticket.size), std::string_view(ticket.price));
}

template<typename Streamer>
void Executor<Streamer>::close(const ExecutorCommand &command, nanoseconds now) noexcept
{
    Seat &seat = m_seats[command.slot];

    if (!seat.bound)
    {
        if (command.type == ExecutorCommandType::Release)
        {
            emit(AccountEventType::Released, command.slot, command.episode, 0.0, 0.0, 0, PositionSide::Long, false);
        }

        return;
    }

    if (seat.episode != command.episode) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx executor close ignored: slot = {}, episode = {}, bound episode = {}", command.slot, command.episode, seat.episode);
        return;
    }

    seat.closing = true;
    seat.releasing = seat.releasing || command.type == ExecutorCommandType::Release;
    seat.attempts = 0;

    m_log.log("okx executor closing: slot = {}, episode = {}, instrument = {}, release = {}", command.slot, command.episode,
              std::string_view(seat.instrument), seat.releasing);

    flatten(command.slot, now);
    settle(command.slot, now);
}

template<typename Streamer>
void Executor<Streamer>::orders(std::span<const Order> orders, nanoseconds now) noexcept
{
    for (const Order &order : orders)
    {
        const std::string_view state(order.state);
        Ticket *const ticket = find_ticket(order.client);

        if (ticket == nullptr)
        {
            const bool live = !terminal(state);
            const Known *const known = find_known(order.instrument);

            if (live && known != nullptr && order.client[0] != '\0')
            {
                Ticket *const foreign = allocate();
                if (foreign != nullptr)
                {
                    nlx::detail::str_assign(foreign->client, std::string_view(order.client));
                    nlx::detail::str_assign(foreign->instrument, std::string_view(order.instrument));
                    foreign->code = known->code;
                    foreign->slot = kNone;
                    foreign->size = order.size;
                    foreign->filled = order.filled;
                    foreign->sent = now;
                    foreign->acknowledged = true;
                    foreign->used = true;
                }
            }

            if (live)
            {
                m_log.log<logging::LogLevel::Warning>("okx executor foreign order: instrument = {}, client = {}, state = {}, managed = {}",
                                                      std::string_view(order.instrument), std::string_view(order.client), state, known != nullptr);
            }

            continue;
        }

        ticket->acknowledged = true;
        nlx::detail::str_assign(ticket->id, std::string_view(order.id));

        const double delta = order.filled - ticket->filled;
        const size_t slot = ticket->slot;

        if (order.updated != 0)
        {
            m_push.record(time::now() - milliseconds(order.updated));
        }

        const bool first = delta > 0.0 && ticket->filled <= 0.0;
        if (first)
        {
            m_fill.record(now - ticket->sent);
        }

        if (delta > 0.0)
        {
            ticket->filled = order.filled;
            ticket->price = order.price;

            if (slot != kNone)
            {
                Seat &seat = m_seats[slot];
                double &exposure = seat.exposure[side_index(ticket->side)];
                exposure = std::max(0.0, exposure + (ticket->closing ? -delta : delta));
                seat.activity = now;

                emit(AccountEventType::Filled, slot, ticket->episode, ticket->filled, ticket->price, 0, ticket->side, ticket->closing);
            }

            m_log.log("okx executor fill: slot = {}, client = {}, side = {}, closing = {}, filled = {}, price = {}, fee = {}",
                      slot, std::string_view(ticket->client), side_name(ticket->side), ticket->closing, order.filled, order.price, order.fee);
        }

        if (terminal(state))
        {
            m_log.log("okx executor order done: slot = {}, client = {}, state = {}, filled = {}, size = {}, pnl = {}",
                      slot, std::string_view(ticket->client), state, order.filled, order.size, order.pnl);

            const bool unfilled = slot != kNone && !ticket->closing && ticket->filled <= 0.0;
            if (unfilled)
            {
                emit(AccountEventType::Rejected, slot, ticket->episode, 0.0, 0.0, 0, ticket->side, false);
            }

            *ticket = Ticket{};
        }
        else if (slot != kNone && m_seats[slot].closing && !ticket->closing)
        {
            withdraw(*ticket, now);
        }

        if (slot != kNone)
        {
            settle(slot, now);
        }
    }
}

template<typename Streamer>
void Executor<Streamer>::positions(std::span<const Position> positions, bool snapshot, nanoseconds now) noexcept
{
    if (snapshot)
    {
        for (Holding &holding : m_holdings)
        {
            holding.size = 0.0;
        }
    }

    for (const Position &position : positions)
    {
        const std::string_view side(position.side);

        const bool hedged = side == "long" || side == "short";
        if (!hedged) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx executor position in net mode ignored: instrument = {}, size = {}", std::string_view(position.instrument), position.size);
            continue;
        }

        const PositionSide direction = side == "long" ? PositionSide::Long : PositionSide::Short;

        Holding *holding = find_holding(position.instrument, direction);
        if (holding == nullptr)
        {
            for (Holding &candidate : m_holdings)
            {
                if (!candidate.used)
                {
                    holding = &candidate;
                    break;
                }
            }
        }

        if (holding == nullptr) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Error>("okx executor holdings are full: instrument = {}", std::string_view(position.instrument));
            continue;
        }

        const double size = std::abs(position.size);
        if (size <= 0.0)
        {
            *holding = Holding{};
            continue;
        }

        nlx::detail::str_assign(holding->instrument, std::string_view(position.instrument));
        holding->side = direction;
        holding->size = size;
        holding->price = position.price;
        holding->used = true;
    }

    for (Holding &holding : m_holdings)
    {
        if (holding.used && holding.size <= 0.0)
        {
            holding = Holding{};
        }
    }

    for (size_t slot = 0; slot < kSlotsCapacity; ++slot)
    {
        settle(slot, now);
    }

    if (m_draining)
    {
        sweep(now);
    }
}

template<typename Streamer>
void Executor<Streamer>::reply(const OrderReply &reply, nanoseconds now) noexcept
{
    const std::string_view operation(reply.operation);

    Ticket *const ticket = find_ticket(reply.client);
    if (ticket == nullptr)
    {
        if (reply.status != 0)
        {
            m_log.log<logging::LogLevel::Warning>("okx executor reply without order: operation = {}, code = {}, status = {}, message = {}",
                                                  operation, reply.code, reply.status, std::string_view(reply.message));
        }

        return;
    }

    const double gateway = static_cast<double>(reply.sent - reply.received) / 1000.0;
    const size_t slot = ticket->slot;

    const bool stamped = reply.received != 0 && reply.sent != 0;
    if (stamped)
    {
        m_gateway.record(microseconds(reply.sent - reply.received));
    }

    if (operation == "order")
    {
        const double elapsed = detail::ms(now - ticket->sent);
        m_order_trip.record(now - ticket->sent);

        if (reply.status == 0)
        {
            ticket->acknowledged = true;
            nlx::detail::str_assign(ticket->id, std::string_view(reply.id));

            m_log.log("okx executor order accepted: slot = {}, client = {}, rtt = {} ms, gateway = {} ms", slot, std::string_view(ticket->client), elapsed, gateway);

            const bool withdrawing = slot != kNone && m_seats[slot].closing && !ticket->closing;
            if (withdrawing)
            {
                withdraw(*ticket, now);
            }

            return;
        }

        m_log.log<logging::LogLevel::Warning>("okx executor order rejected: slot = {}, client = {}, closing = {}, status = {}, message = {}, rtt = {} ms",
                                              slot, std::string_view(ticket->client), ticket->closing, reply.status, std::string_view(reply.message), elapsed);
        ++m_rejects;

        const bool nothing = ticket->closing && reply.status == 51169;
        if (nothing && slot != kNone)
        {
            m_seats[slot].exposure[side_index(ticket->side)] = 0.0;
        }

        if (!ticket->closing && slot != kNone)
        {
            emit(AccountEventType::Rejected, slot, ticket->episode, 0.0, 0.0, reply.status, ticket->side, false);
        }

        *ticket = Ticket{};

        if (slot != kNone)
        {
            settle(slot, now);
        }

        return;
    }

    m_cancel_trip.record(now - ticket->cancelled);

    const bool finished = reply.status == 51400 || reply.status == 51401 || reply.status == 51402 || reply.status == 51410;
    if (finished)
    {
        m_log.log("okx executor cancel skipped: slot = {}, client = {}, status = {}, order is already done or cancelling, waiting for push",
                  slot, std::string_view(ticket->client), reply.status);
        return;
    }

    if (reply.status != 0)
    {
        m_log.log<logging::LogLevel::Warning>("okx executor cancel failed: slot = {}, client = {}, status = {}, message = {}",
                                              slot, std::string_view(ticket->client), reply.status, std::string_view(reply.message));
    }
}

template<typename Streamer>
void Executor<Streamer>::report() noexcept
{
    m_log.log<logging::LogLevel::Metric>("executor metrics: orders = {}, rejects = {}, order rtt p50 = {} ms, p99 = {} ms, max = {} ms, gateway p50 = {} ms, p99 = {} ms",
              m_order_trip.count(), m_rejects, m_order_trip.percentile(0.5) / 1000.0, m_order_trip.percentile(0.99) / 1000.0,
              m_order_trip.max() / 1000.0, m_gateway.percentile(0.5) / 1000.0, m_gateway.percentile(0.99) / 1000.0);
    m_log.log<logging::LogLevel::Metric>("executor metrics: first fill p50 = {} ms, p99 = {} ms, cancel rtt p50 = {} ms, order push delay p50 = {} ms, p99 = {} ms",
              m_fill.percentile(0.5) / 1000.0, m_fill.percentile(0.99) / 1000.0, m_cancel_trip.percentile(0.5) / 1000.0,
              m_push.percentile(0.5) / 1000.0, m_push.percentile(0.99) / 1000.0);

    m_order_trip.reset();
    m_cancel_trip.reset();
    m_gateway.reset();
    m_fill.reset();
    m_push.reset();
    m_rejects = 0;
}

template<typename Streamer>
void Executor<Streamer>::tick(nanoseconds now) noexcept
{
    for (Ticket &ticket : m_tickets)
    {
        if (!ticket.used)
        {
            continue;
        }

        const bool lost = !ticket.acknowledged && now - ticket.sent >= milliseconds(kReplyMilliseconds);
        if (lost) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx executor order lost without reply: slot = {}, client = {}", ticket.slot, std::string_view(ticket.client));

            const size_t slot = ticket.slot;
            ticket = Ticket{};

            if (slot != kNone)
            {
                settle(slot, now);
            }

            continue;
        }

        const bool repeat = ticket.cancelled != nanoseconds::zero() && now - ticket.cancelled >= milliseconds(kCancelMilliseconds);
        if (repeat)
        {
            withdraw(ticket, now);
        }

        const bool stuck = ticket.closing && ticket.acknowledged && ticket.cancelled == nanoseconds::zero() && now - ticket.sent >= milliseconds(kStuckMilliseconds);
        if (stuck) [[unlikely]]
        {
            m_log.log<logging::LogLevel::Warning>("okx executor close order stuck on the exchange, replacing: slot = {}, client = {}, filled = {}, size = {}",
                                                  ticket.slot, std::string_view(ticket.client), ticket.filled, ticket.size);
            withdraw(ticket, now);
        }
    }

    for (size_t slot = 0; slot < kSlotsCapacity; ++slot)
    {
        const Seat &seat = m_seats[slot];

        const bool retry = seat.bound && seat.closing && now - seat.flattened >= milliseconds(kRetryMilliseconds);
        if (retry)
        {
            flatten(slot, now);
        }

        settle(slot, now);
    }

    if (m_draining)
    {
        sweep(now);
    }
}

template<typename Streamer>
void Executor<Streamer>::drain(nanoseconds now) noexcept
{
    m_draining = true;

    m_log.log("okx executor draining: cancel every order and close every position");

    for (size_t slot = 0; slot < kSlotsCapacity; ++slot)
    {
        Seat &seat = m_seats[slot];
        if (!seat.bound)
        {
            continue;
        }

        seat.closing = true;
        seat.releasing = true;

        flatten(slot, now);
    }

    sweep(now);
}

template<typename Streamer>
bool Executor<Streamer>::flat() const noexcept
{
    for (const Ticket &ticket : m_tickets)
    {
        if (ticket.used)
        {
            return false;
        }
    }

    for (const Holding &holding : m_holdings)
    {
        if (holding.used && holding.size > 0.0)
        {
            return false;
        }
    }

    for (const Seat &seat : m_seats)
    {
        if (seat.bound)
        {
            return false;
        }
    }

    return true;
}

template<typename Streamer>
bool Executor<Streamer>::pop(AccountEvent &event) noexcept
{
    if (m_events_head == m_events_tail)
    {
        return false;
    }

    event = m_events[m_events_head % kEventsCapacity];
    ++m_events_head;

    return true;
}

template<typename Streamer>
void Executor<Streamer>::flatten(size_t slot, nanoseconds now) noexcept
{
    Seat &seat = m_seats[slot];
    seat.flattened = now;
    ++seat.attempts;

    const bool waiting = working(slot, PositionSide::Long, true) || working(slot, PositionSide::Short, true);
    if (seat.attempts > 5 && waiting) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx executor close order still working on the exchange: slot = {}, instrument = {}, attempts = {}, long = {}, short = {}",
                                              slot, std::string_view(seat.instrument), seat.attempts, held(seat.instrument, PositionSide::Long),
                                              held(seat.instrument, PositionSide::Short));
    }

    if (seat.attempts > 5 && !waiting) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx executor still not flat: slot = {}, instrument = {}, attempts = {}, long = {}, short = {}",
                                            slot, std::string_view(seat.instrument), seat.attempts, held(seat.instrument, PositionSide::Long),
                                            held(seat.instrument, PositionSide::Short));
    }

    for (Ticket &ticket : m_tickets)
    {
        const bool opening = ticket.used && ticket.slot == slot && !ticket.closing;
        if (opening && ticket.acknowledged && ticket.cancelled == nanoseconds::zero())
        {
            withdraw(ticket, now);
        }
    }

    for (const PositionSide side : { PositionSide::Long, PositionSide::Short })
    {
        const double amount = std::max(held(seat.instrument, side), seat.exposure[side_index(side)]);

        const bool open = amount > seat.lot / 2.0 && !working(slot, side, true);
        if (open)
        {
            liquidate(slot, seat.instrument, seat.code, seat.lot, side, amount, now);
        }
    }
}

template<typename Streamer>
void Executor<Streamer>::sweep(nanoseconds now) noexcept
{
    for (Holding &holding : m_holdings)
    {
        if (!holding.used || holding.size <= 0.0)
        {
            continue;
        }

        bool owned = false;
        for (const Seat &seat : m_seats)
        {
            owned = owned || (seat.bound && std::string_view(seat.instrument) == std::string_view(holding.instrument));
        }

        if (owned)
        {
            continue;
        }

        bool closing = false;
        for (const Ticket &ticket : m_tickets)
        {
            closing = closing || (ticket.used && ticket.closing && ticket.side == holding.side && std::string_view(ticket.instrument) == std::string_view(holding.instrument));
        }

        if (closing)
        {
            continue;
        }

        const Known *const known = find_known(holding.instrument);
        if (known == nullptr && !holding.reported)
        {
            m_log.log<logging::LogLevel::Error>("okx executor unmanaged position, close it by hand: instrument = {}, side = {}, size = {}",
                                                std::string_view(holding.instrument), side_name(holding.side), holding.size);
            holding.reported = true;
        }

        if (known == nullptr)
        {
            continue;
        }

        liquidate(kNone, holding.instrument, known->code, known->lot, holding.side, holding.size, now);
    }

    for (Ticket &ticket : m_tickets)
    {
        const bool foreign = ticket.used && ticket.slot == kNone && !ticket.closing && ticket.cancelled == nanoseconds::zero();
        if (foreign)
        {
            withdraw(ticket, now);
        }
    }
}

template<typename Streamer>
void Executor<Streamer>::settle(size_t slot, nanoseconds now) noexcept
{
    Seat &seat = m_seats[slot];
    if (!seat.bound || !seat.closing)
    {
        return;
    }

    for (const Ticket &ticket : m_tickets)
    {
        if (ticket.used && ticket.slot == slot)
        {
            return;
        }
    }

    const bool holding = held(seat.instrument, PositionSide::Long) > 0.0 || held(seat.instrument, PositionSide::Short) > 0.0;
    if (holding)
    {
        return;
    }

    const bool exposed = seat.exposure[0] > seat.lot / 2.0 || seat.exposure[1] > seat.lot / 2.0;
    const bool waited = now - seat.activity >= milliseconds(kSettleMilliseconds);

    if (exposed && !waited)
    {
        return;
    }

    if (exposed) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx executor exposure disagrees with positions, trusting the exchange: slot = {}, long = {}, short = {}",
                                              slot, seat.exposure[0], seat.exposure[1]);
    }

    seat.closing = false;
    seat.attempts = 0;
    seat.exposure = { 0.0, 0.0 };

    m_log.log("okx executor flat: slot = {}, episode = {}, instrument = {}", slot, seat.episode, std::string_view(seat.instrument));
    emit(AccountEventType::Flat, slot, seat.episode, 0.0, 0.0, 0, PositionSide::Long, false);

    if (seat.releasing)
    {
        emit(AccountEventType::Released, slot, seat.episode, 0.0, 0.0, 0, PositionSide::Long, false);
        seat = Seat{};
    }
}

template<typename Streamer>
void Executor<Streamer>::liquidate(size_t slot, const char *instrument, int64_t code, double lot, PositionSide side, double amount, nanoseconds now) noexcept
{
    OrderTicket ticket{};
    ticket.instrument = code;
    ticket.position = side;
    ticket.type = OrderType::Market;
    ticket.buy = side == PositionSide::Short;

    Ticket *const target = format_step(amount, lot, true, ticket.size) ? allocate() : nullptr;
    if (target == nullptr) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx executor close not prepared: slot = {}, instrument = {}, amount = {}", slot, std::string_view(instrument), amount);
        return;
    }

    const uint64_t episode = slot != kNone ? m_seats[slot].episode : 0;

    const auto written = std::format_to_n(target->client, sizeof(target->client) - 1, "n{}e{}c{}", m_tag, episode, ++m_sequence);
    target->client[written.size] = '\0';

    std::copy_n(target->client, sizeof(ticket.client), ticket.client);

    nlx::detail::str_assign(target->instrument, std::string_view(instrument));
    target->code = code;
    target->slot = slot;
    target->episode = episode;
    target->size = amount;
    target->sent = now;
    target->side = side;
    target->closing = true;
    target->used = true;

    if (!m_streamer.place(ticket)) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Warning>("okx executor close not sent, retrying later: slot = {}, client = {}", slot, std::string_view(ticket.client));

        *target = Ticket{};
        return;
    }

    m_log.log("okx executor close sent: slot = {}, client = {}, instrument = {}, side = {}, size = {}",
              slot, std::string_view(ticket.client), std::string_view(instrument), side_name(side), std::string_view(ticket.size));
}

template<typename Streamer>
void Executor<Streamer>::withdraw(Ticket &ticket, nanoseconds now) noexcept
{
    if (!ticket.acknowledged)
    {
        return;
    }

    if (m_streamer.cancel(ticket.code, ticket.client))
    {
        ticket.cancelled = now;
        m_log.log("okx executor cancel sent: slot = {}, client = {}", ticket.slot, std::string_view(ticket.client));
    }
}

template<typename Streamer>
typename Executor<Streamer>::Ticket *Executor<Streamer>::allocate() noexcept
{
    for (Ticket &ticket : m_tickets)
    {
        if (!ticket.used)
        {
            ticket = Ticket{};
            return &ticket;
        }
    }

    return nullptr;
}

template<typename Streamer>
typename Executor<Streamer>::Ticket *Executor<Streamer>::find_ticket(std::string_view client) noexcept
{
    if (client.empty())
    {
        return nullptr;
    }

    for (Ticket &ticket : m_tickets)
    {
        if (ticket.used && std::string_view(ticket.client) == client)
        {
            return &ticket;
        }
    }

    return nullptr;
}

template<typename Streamer>
typename Executor<Streamer>::Holding *Executor<Streamer>::find_holding(std::string_view instrument, PositionSide side) noexcept
{
    for (Holding &holding : m_holdings)
    {
        if (holding.used && holding.side == side && std::string_view(holding.instrument) == instrument)
        {
            return &holding;
        }
    }

    return nullptr;
}

template<typename Streamer>
const typename Executor<Streamer>::Known *Executor<Streamer>::find_known(std::string_view instrument) const noexcept
{
    for (size_t index = 0; index < m_known_size; ++index)
    {
        if (std::string_view(m_known[index].instrument) == instrument)
        {
            return &m_known[index];
        }
    }

    return nullptr;
}

template<typename Streamer>
bool Executor<Streamer>::working(size_t slot, PositionSide side, bool closing) const noexcept
{
    for (const Ticket &ticket : m_tickets)
    {
        if (ticket.used && ticket.slot == slot && ticket.side == side && ticket.closing == closing)
        {
            return true;
        }
    }

    return false;
}

template<typename Streamer>
double Executor<Streamer>::held(std::string_view instrument, PositionSide side) const noexcept
{
    for (const Holding &holding : m_holdings)
    {
        if (holding.used && holding.side == side && std::string_view(holding.instrument) == instrument)
        {
            return holding.size;
        }
    }

    return 0.0;
}

template<typename Streamer>
void Executor<Streamer>::emit(AccountEventType type, size_t slot, uint64_t episode, double size, double price, int code, PositionSide side, bool closing) noexcept
{
    if (m_events_tail - m_events_head == kEventsCapacity) [[unlikely]]
    {
        m_log.log<logging::LogLevel::Error>("okx executor event dropped: queue is full, slot = {}", slot);
        return;
    }

    m_events[m_events_tail % kEventsCapacity] = AccountEvent{ type, slot, episode, Balance{}, size, price, code, side, closing };
    ++m_events_tail;
}

template class Executor<exchange::OkxAccountStreamer>;

} // namespace nlx::service
