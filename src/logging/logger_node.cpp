#include "logging/logger_node.h"

#include <utility>

namespace nlx::logging
{

LoggerNode::LoggerNode() noexcept :
    m_notify(nullptr), m_channel(nullptr)
{

}

LoggerNode::LoggerNode(LoggerChannel &channel, const FileDescriptor &notify) noexcept :
    m_notify(&notify), m_channel(&channel)
{

}

LoggerNode::~LoggerNode() noexcept
{
    flush();
}

LoggerNode::LoggerNode(LoggerNode &&other) noexcept :
    m_notify(std::exchange(other.m_notify, nullptr)),
    m_channel(std::exchange(other.m_channel, nullptr))
{

}

LoggerNode &LoggerNode::operator=(LoggerNode &&other) noexcept
{
    if (this != &other)
    {
        flush();

        m_notify = std::exchange(other.m_notify, nullptr);
        m_channel = std::exchange(other.m_channel, nullptr);
    }

    return *this;
}

void LoggerNode::flush() noexcept
{
    if (m_channel == nullptr || m_notify == nullptr)
    {
        return;
    }

    if (!m_channel->queue.empty_approx()) 
    {
        nlx::detail::notify(*m_notify);
    }
}

} // namespace nlx::logging
