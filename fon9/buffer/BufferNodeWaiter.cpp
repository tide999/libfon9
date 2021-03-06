﻿// \file fon9/BufferNodeWaiter.cpp
// \author fonwinz@gmail.com
#include "fon9/buffer/BufferNodeWaiter.hpp"

namespace fon9 {

BufferNodeWaiter::~BufferNodeWaiter() {
}
BufferNodeWaiter* BufferNodeWaiter::Alloc(CountDownLatch& waiter) {
   BufferNodeWaiter* res = base::Alloc<BufferNodeWaiter>(0, StyleFlag::AllowCrossing);
   res->Waiter_ = &waiter;
   return res;
}
void BufferNodeWaiter::OnBufferConsumed() {
   this->Waiter_->CountDown();
}
void BufferNodeWaiter::OnBufferConsumedErr(const ErrC&) {
   this->Waiter_->CountDown();
}

} // namespaces
