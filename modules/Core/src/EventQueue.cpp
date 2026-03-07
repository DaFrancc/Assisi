/// @file EventQueue.cpp

#include <Assisi/Core/EventQueue.hpp>

namespace Assisi::Core
{

EventQueue &EventQueue::Instance()
{
    static EventQueue instance;
    return instance;
}

} // namespace Assisi::Core