#ifndef NLX_CORE_ORDER_BOOK_H
#define NLX_CORE_ORDER_BOOK_H

#include "core/market.h"

#include <array>
#include <cstddef>

namespace nlx
{

class OrderBook
{
public:
    static constexpr size_t kDepth = 400;

    OrderBook() noexcept;

    void clear() noexcept;

    void bid(double price, double size) noexcept;
    void ask(double price, double size) noexcept;

    [[nodiscard]] double best_bid() const noexcept;
    [[nodiscard]] double best_ask() const noexcept;
    [[nodiscard]] bool crossed() const noexcept;

    [[nodiscard]] size_t bids_size() const noexcept;
    [[nodiscard]] size_t asks_size() const noexcept;

private:
    std::array<Level, kDepth> m_bids;
    std::array<Level, kDepth> m_asks;

    size_t m_bids_size;
    size_t m_asks_size;
};

} // namespace nlx
#endif // !NLX_CORE_ORDER_BOOK_H
