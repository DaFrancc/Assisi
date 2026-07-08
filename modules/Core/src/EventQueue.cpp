/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
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