#include "core/order_book.h"

#include <algorithm>
#include <functional>

namespace nlx
{

namespace
{

template<typename Better>
void update(std::array<Level, OrderBook::kDepth> &levels, size_t &size, double price, double amount, Better better) noexcept
{
    size_t low = 0;
    size_t high = size;

    while (low < high)
    {
        const size_t middle = (low + high) / 2;
        if (better(levels[middle].price, price))
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }

    Level *const begin = levels.data();
    Level *const found = begin + low;

    const bool existing = low != size && found->price == price;

    if (amount == 0.0)
    {
        if (existing)
        {
            std::copy(found + 1, begin + size, found);
            --size;
        }

        return;
    }

    if (existing)
    {
        found->size = amount;
        return;
    }

    if (low == OrderBook::kDepth)
    {
        return;
    }

    const size_t last = std::min(size, OrderBook::kDepth - 1);
    std::copy_backward(found, begin + last, begin + last + 1);

    *found = Level{ price, amount };
    size = last + 1;
}

} // namespace

OrderBook::OrderBook() noexcept :
    m_bids(),
    m_asks(),
    m_bids_size(0),
    m_asks_size(0)
{

}

void OrderBook::clear() noexcept
{
    m_bids_size = 0;
    m_asks_size = 0;
}

void OrderBook::bid(double price, double size) noexcept
{
    update(m_bids, m_bids_size, price, size, std::greater<double>());
}

void OrderBook::ask(double price, double size) noexcept
{
    update(m_asks, m_asks_size, price, size, std::less<double>());
}

double OrderBook::best_bid() const noexcept
{
    return m_bids_size != 0 ? m_bids[0].price : 0.0;
}

double OrderBook::best_ask() const noexcept
{
    return m_asks_size != 0 ? m_asks[0].price : 0.0;
}

bool OrderBook::crossed() const noexcept
{
    return m_bids_size != 0 && m_asks_size != 0 && m_bids[0].price >= m_asks[0].price;
}

size_t OrderBook::bids_size() const noexcept
{
    return m_bids_size;
}

size_t OrderBook::asks_size() const noexcept
{
    return m_asks_size;
}

} // namespace nlx
