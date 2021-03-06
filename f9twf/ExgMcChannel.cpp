﻿// \file f9twf/ExgMcChannel.cpp
// \author fonwinz@gmail.com
#include "f9twf/ExgMcChannel.hpp"
#include "f9twf/ExgMdPkReceiver.hpp"
#include "f9twf/ExgMcFmtSS.hpp"
#include "f9twf/ExgMdFmtBasicInfo.hpp"
#include "f9twf/ExgMrRecover.hpp"
#include "fon9/FileReadAll.hpp"
#include "fon9/Log.hpp"

namespace f9twf {

ExgMcMessageConsumer::~ExgMcMessageConsumer() {
}
//--------------------------------------------------------------------------//
void ExgMcChannel::StartupChannel(std::string logPath) {
   this->State_ = (IsEnumContainsAny(this->Style_, ExgMcChannelStyle::WaitBasic | ExgMcChannelStyle::WaitSnapshot)
                   ? ExgMcChannelState::Waiting : ExgMcChannelState::Running);
   this->CycleStartSeq_ = 0;
   this->CycleBeforeLostCount_ = 0;
   this->Pk1stPos_ = 0;
   this->Pk1stSeq_ = 0;
   this->Clear();
   this->PkLog_.reset();
   if (IsEnumContainsAny(this->Style_, ExgMcChannelStyle::PkLog | ExgMcChannelStyle::Reload)) {
      fon9::NumOutBuf nbuf;
      logPath.append(fon9::ToStrRev(nbuf.end(), this->ChannelId_, fon9::FmtDef{4, fon9::FmtFlag::IntPad0}),
                     nbuf.end());
      logPath.append(".bin");
      this->PkLog_ = fon9::AsyncFileAppender::Make();
      auto res = this->PkLog_->OpenImmediately(logPath, fon9::FileMode::CreatePath | fon9::FileMode::Read | fon9::FileMode::Append);
      if (res.HasResult()) {
         res = this->PkLog_->GetFileSize();
         if (res.HasResult())
            this->Pk1stPos_ = res.GetResult();
      }
      if (!res.HasResult()) {
         this->PkLog_.reset();
         fon9_LOG_FATAL(this->ChannelMgr_->Name_, ".StartupChannel|channelId=", this->ChannelId_, "|fn=", logPath, '|', res);
      }
   }
   if (this->IsSnapshot()) // 快照更新, 尚未收到 A:Refresh Begin 之前, 不記錄 PkLog.
      this->IsSkipPkLog_ = true;
}
void ExgMcChannel::SetupReload() {
   if (!this->PkLog_ || !IsEnumContains(this->Style_, ExgMcChannelStyle::Reload))
      return;
   struct McReloader : public ExgMcPkReceiver {
      fon9_NON_COPY_NON_MOVE(McReloader);
      ExgMcChannel&           Owner_;
      const ExgMcChannelState BfState_;
      char                    Padding___[7];
      McReloader(ExgMcChannel& owner) : Owner_(owner), BfState_{owner.State_} {
         owner.IsSkipPkLog_ = owner.IsSetupReloading_ = true;
      }
      ~McReloader() {
         this->Owner_.IsSkipPkLog_ = this->Owner_.IsSetupReloading_ = false;
      }
      bool operator()(fon9::DcQueue& rdbuf, fon9::File::Result&) {
         this->FeedBuffer(rdbuf);
         return true;
      }
      bool OnPkReceived(const void* pk, unsigned pksz) override {
         this->Owner_.OnPkReceived(*static_cast<const ExgMcHead*>(pk), pksz);
         return true;
      }
   };
   McReloader           reloader{*this};
   fon9::File::SizeType fpos = 0;
   fon9::File::Result   res = fon9::FileReadAll(*this->PkLog_, fpos, reloader);
   if (res.IsError())
      fon9_LOG_FATAL(this->ChannelMgr_->Name_, ".SetupReload|channelId=", this->ChannelId_, "|pos=", fpos, '|', res);
}
void ExgMcChannel::ReloadDispatch(SeqT fromSeq) {
   if (!this->PkLog_)
      return;
   struct McReloader : public ExgMcPkReceiver {
      fon9_NON_COPY_NON_MOVE(McReloader);
      ExgMcChannel&        Owner_;
      const SeqT           FromSeq_;
      PkPendings::Locker   Locker_;
      McReloader(ExgMcChannel& owner, SeqT fromSeq)
         : Owner_(owner)
         , FromSeq_{fromSeq}
         , Locker_{owner.PkPendings_.Lock()} {
      }
      bool operator()(fon9::DcQueue& rdbuf, fon9::File::Result&) {
         this->FeedBuffer(rdbuf);
         return true;
      }
      bool OnPkReceived(const void* pk, unsigned pksz) override {
         SeqT seq = static_cast<const ExgMcHead*>(pk)->GetChannelSeq();
         if (this->FromSeq_ == 0 || seq >= this->FromSeq_) {
            ExgMcMessage e(*static_cast<const ExgMcHead*>(pk), pksz, this->Owner_, seq);
            this->Owner_.DispatchMcMessage(e);
         }
         return true;
      }
   };
   McReloader           reloader{*this, fromSeq};
   fon9::File::SizeType fpos = this->Pk1stPos_;
   fon9::File::Result   res = fon9::FileReadAll(*this->PkLog_, fpos, reloader);
   if (res.IsError())
      fon9_LOG_FATAL(this->ChannelMgr_->Name_, ".ReloadDispatch|channelId=", this->ChannelId_, "|pos=", fpos, '|', res);
   if (this->State_ < ExgMcChannelState::Cycled)
      this->State_ = ExgMcChannelState::Running;
}
void ExgMcChannel::OnBasicInfoCycled() {
   if (!IsEnumContains(this->Style_, ExgMcChannelStyle::WaitBasic)
       || this->State_ != ExgMcChannelState::Waiting)
      return;
   if (IsEnumContains(this->Style_, ExgMcChannelStyle::WaitSnapshot)) {
      if (!this->ChannelMgr_->IsSnapshotDone(*this))
         return;
   }
   this->ReloadDispatch(0);
}
void ExgMcChannel::OnSnapshotDone(SeqT lastRtSeq) {
   if (IsEnumContains(this->Style_, ExgMcChannelStyle::WaitSnapshot)
       && this->State_ == ExgMcChannelState::Waiting)
      this->ReloadDispatch(this->IsRealtime() ? lastRtSeq + 1 : 0);
}
//--------------------------------------------------------------------------//
void ExgMcChannel::AddRecover(ExgMrRecoverSessionSP svr) {
   auto rs = this->Recovers_.Lock();
   for (auto& cur : *rs) {
      if (cur == svr)
         return;
   }
   if (svr->Role_ == ExgMcRecoverRole::Primary)
      rs->push_front(svr);
   else
      rs->push_back(svr);
}
void ExgMcChannel::DelRecover(ExgMrRecoverSessionSP svr) {
   bool isFront;
   {
      auto rs = this->Recovers_.Lock();
      auto ifind = std::find(rs->begin(), rs->end(), svr);
      if (ifind == rs->end())
         return;
      isFront = (ifind == rs->begin());
      rs->erase(ifind);
   }
   if (isFront && !this->PkPendings_.Lock()->empty())
      this->Timer_.RunAfter(this->WaitInterval_);
}
void ExgMcChannel::OnRecoverErr(SeqT beginSeqNo, unsigned recoverNum) {
   auto pks = this->PkPendings_.Lock();
   if (pks->empty())
      return;
   if (beginSeqNo + recoverNum >= pks->begin()->Seq_)
      base::PkContOnTimer(std::move(pks));
}
void ExgMcChannel::PkContOnTimer(PkPendings::Locker&& pks) {
   const auto keeps = pks->size();
   if (keeps == 0)
      return;
   const bool  isKeepsNoGap = (keeps == 1 || (pks->back().Seq_ - pks->begin()->Seq_ + 1 == keeps));
   SeqT        recoverNum = (isKeepsNoGap ? pks->begin()->Seq_ : pks->back().Seq_) - this->NextSeq_;
   ExgMrRecoverSessionSP svr;
   {
      auto rs = this->Recovers_.Lock();
      if (!rs->empty())
         svr = rs->front();
   }
   if (fon9_UNLIKELY(fon9::LogLevel::Warn >= fon9::LogLevel_)) {
      fon9::RevBufferList rbuf_{fon9::kLogBlockNodeSize};
      fon9::RevPrint(rbuf_, "|keeps=", keeps, '\n');
      if (!isKeepsNoGap)
         fon9::RevPrint(rbuf_, "|back=", pks->back().Seq_);
      fon9::RevPrint(rbuf_, "|channelId=", this->ChannelId_,
                     "|from=", this->NextSeq_, "|to=", pks->begin()->Seq_ - 1);
      if (svr)
         fon9::RevPrint(rbuf_, "|recoverSessionId=", svr->SessionId_,
                        "|requestCount=", svr->GetRequestCount(),
                        "|recoverNum=", recoverNum);
      fon9::RevPrint(rbuf_, this->ChannelMgr_->Name_, ".PkLost");
      fon9::LogWrite(fon9::LogLevel::Warn, std::move(rbuf_));
   }
   /// 每次最多回補 1000 筆, 每個 Session 一天最多可以要求 1000 次.
   const ExgMrRecoverNum_t kMaxRecoverNum = 1000;
   const ExgMrRecoverNum_t kMaxRequestCount = 1000;
   if (svr) {
      ExgMrMsgSeqNum_t from = static_cast<ExgMrMsgSeqNum_t>(this->NextSeq_);
      for (;;) {
         if (svr->GetRequestCount() < kMaxRequestCount) {
            ExgMrRecoverNum_t reNum = (recoverNum > kMaxRecoverNum ? kMaxRecoverNum : static_cast<ExgMrRecoverNum_t>(recoverNum));
            if (svr->RequestMcRecover(this->ChannelId_, from, reNum)) {
               if ((recoverNum -= reNum) <= 0)
                  return;
               from += reNum;
               continue;
            }
         }
         auto rs = this->Recovers_.Lock();
         if (rs->empty())
            break;
         ExgMrRecoverSessionSP beg = *rs->begin();
         if (svr != beg)
            svr.swap(beg);
         else {
            rs->pop_front();
            if (rs->empty())
               break;
            svr = *rs->begin();
         }
         fon9_LOG_WARN(this->ChannelMgr_->Name_,
                       "|recoverSessionId=", svr->SessionId_,
                       "|requestCount=", svr->GetRequestCount());
      }
      fon9_LOG_WARN(this->ChannelMgr_->Name_, ".PkLost|err=NoRecover");
   }
   base::PkContOnTimer(std::move(pks));
}
// -----
// 收到完整封包後, 會執行到此, 尚未確定是否重複或有遺漏.
ExgMcChannelState ExgMcChannel::OnPkReceived(const ExgMcHead& pk, unsigned pksz) {
   assert(pk.GetChannelId() == this->ChannelId_);
   const SeqT seq = pk.GetChannelSeq();
   // ----- 處理特殊訊息: Hb, SeqReset...
   if (fon9_UNLIKELY(pk.TransmissionCode_ == '0')) {
      switch (pk.MessageKind_) {
      case '1':
         if (this->IsSnapshot()) // 快照更新: 使用 (A:Refresh Begin .. Z:Refresh Complete) 判斷 CycledClose.
            return this->State_;
         // Heartbeat: CHANNEL-SEQ 欄位會為該傳輸群組之末筆序號訊息。
         if (this->NextSeq_ != seq + 1) // 序號不正確的 Hb, 不處理.
            return this->State_;
         this->PkLogAppend(&pk, pksz, seq);
         // 檢查 Channel 是否已符合 Cycled 條件.
         if (seq == 0 // 期交所系統已啟動, 但尚未開始發送輪播訊息.
             || this->CycleStartSeq_ == seq + 1) // 兩次輪播循環之間的 Hb: 不用理會.
            return this->State_;
         if (IsEnumContainsAny(this->Style_, ExgMcChannelStyle::CycledClose | ExgMcChannelStyle::CycledNotify)
             && this->CycleStartSeq_
             && this->State_ < ExgMcChannelState::Cycled // ChannelCycled() 事件只觸發一次.
             && this->LostCount_ - this->CycleBeforeLostCount_ == 0) { // 沒有遺漏, 才算是 Cycled 完成.
            this->State_ = (IsEnumContains(this->Style_, ExgMcChannelStyle::CycledClose)
                            ? ExgMcChannelState::CanBeClosed : ExgMcChannelState::Cycled);
            this->ChannelMgr_->ChannelCycled(*this);
         }
         this->OnCycleStart(seq + 1);
         return this->State_;
      case '2': // SeqReset.
         this->NextSeq_ = 0;
         if (0);//SeqReset 之後, API 商品訂閱者的序號要怎麼處理呢?
         return this->State_;
      }
   }
   // ----- 如果允許任意順序, 則可先直接處理 DispatchMcMessage().
   if (IsEnumContains(this->Style_, ExgMcChannelStyle::AnySeq)) {
      if (this->State_ != ExgMcChannelState::Waiting) {
         ExgMcMessage e(pk, pksz, *this, seq);
         auto         locker = this->PkPendings_.Lock();
         this->DispatchMcMessage(e);
      }
   }
   // ----- 不允許任意順序 or 有其他 Style, 則應透過 [序號連續] 機制處理.
   if ((this->Style_ - ExgMcChannelStyle::AnySeq) != ExgMcChannelStyle{}
       || this->Style_ == ExgMcChannelStyle{})
      this->FeedPacket(&pk, pksz, seq);
   return this->State_;
}
// -----
// 確定有連續, 或確定無法連續, 會執行到此.
// 此時 this->PkPendings_ 是 locked 狀態.
void ExgMcChannel::PkContOnReceived(const void* pk, unsigned pksz, SeqT seq) {
   if (fon9_UNLIKELY(IsEnumContains(this->Style_, ExgMcChannelStyle::AnySeq))) {
      // AnySeq 在收到封包的第一時間, 就已觸發 this->DispatchMcMessage(e);
      // 所以在確定連續時, 不用再處理了!
   }
   else if (fon9_UNLIKELY(this->IsSnapshot())) {
      if (this->State_ == ExgMcChannelState::CanBeClosed) // 已收過一輪, 不用再收.
         return;
      if (static_cast<const ExgMcHead*>(pk)->MessageKind_ != 'C')
         return; // 不認識的快照訊息?!
      switch (static_cast<const ExgMcHead*>(pk)->TransmissionCode_) {
      case '2': // I084.Fut.
      case '5': // I084.Opt.
         break;
      default: // 不認識的快照訊息?!
         return;
      }
      SeqT lastRtSeq;
      switch (static_cast<const f9twf::ExgMcI084*>(pk)->MessageType_) {
      case 'A':
         lastRtSeq = fon9::PackBcdTo<SeqT>(static_cast<const f9twf::ExgMcI084*>(pk)->_A_RefreshBegin_.LastSeq_);
         if (this->IsSkipPkLog_) {
            // 檢查是否可以開始接收新的一輪: 判斷即時行情的 Pk1stSeq_ 是否符合要求?
            auto rtseq = this->ChannelMgr_->GetRealtimeChannel(*this).Pk1stSeq_;
            // if (lastRtSeq == 0)  // 即時行情開始之前的快照.
            // if (rtseq == 0)      // 尚未收到即時行情.
            if (rtseq > lastRtSeq)
               return;
            this->IsSkipPkLog_ = false;
            this->OnCycleStart(lastRtSeq);
         }
         else { // 還沒收一輪, 但又收到 'A', 表示前一輪接收失敗(至少有漏 'Z') => 重收.
            this->OnCycleStart(lastRtSeq);
            if (this->PkLog_) {
               auto res = this->PkLog_->GetFileSize();
               if (res.HasResult())
                  this->Pk1stPos_ = res.GetResult();
               this->Pk1stSeq_ = seq;
            }
         }
         break;
      case 'Z':
         if (this->IsSkipPkLog_) { // 尚未開始.
            this->AfterNextSeq_ = 0;
            return;
         }
         lastRtSeq = fon9::PackBcdTo<SeqT>(static_cast<const f9twf::ExgMcI084*>(pk)->_Z_RefreshComplete_.LastSeq_);
         if (lastRtSeq == this->CycleStartSeq_) {
            auto rtseq = this->ChannelMgr_->GetRealtimeChannel(*this).Pk1stSeq_;
            // 檢查是否有遺漏? 有遺漏則重收.
            // 沒遺漏則觸發 OnSnapshotDone() 事件.
            if ((rtseq == 1 || rtseq <= lastRtSeq) && this->LostCount_ - this->CycleBeforeLostCount_ == 0) {
               this->State_ = ExgMcChannelState::CanBeClosed;
               if (this->ChannelMgr_->IsBasicInfoCycled(*this))
                  this->ChannelMgr_->OnSnapshotDone(*this, lastRtSeq);
            }
         }
         if (this->State_ != ExgMcChannelState::CanBeClosed) {
            // 收到 'Z' 之後, 若下一個循環的 lastRtSeq 沒變, 則 ChannelSeq 會回頭使用 'A' 的 ChannelSeq.
            // 所以在收到 'Z' 之後, 需要重設 NextSeq_; 因為在漏封包時, 必須可以重收下一次的循環.
            this->AfterNextSeq_ = 0;
         }
         break;
      default:
         if (this->IsSkipPkLog_) // 尚未開始.
            return;
         break;
      }
      goto __DISPATCH_AND_PkLog;
   }
   else {
   __DISPATCH_AND_PkLog:
      if (fon9_LIKELY(State_ != ExgMcChannelState::Waiting)) {
         ExgMcMessage e(*static_cast<const ExgMcHead*>(pk), pksz, *this, seq);
         this->DispatchMcMessage(e);
      }
   }
   this->PkLogAppend(pk, pksz, seq);
}
void ExgMcChannel::DispatchMcMessage(ExgMcMessage& e) {
   // assert(this->PkPending_.IsLocked());
   assert(e.Pk_.GetChannelId() == this->ChannelId_);
   this->ChannelMgr_->DispatchMcMessage(e);
   if (!this->IsSetupReloading_) {
      struct Combiner {
         bool operator()(ExgMcMessageConsumer* subr, ExgMcMessage& e) {
            subr->OnExgMcMessage(e);
            return true;
         }
      } combiner;
      this->UnsafeConsumers_.Combine(combiner, e);
   }
}
//--------------------------------------------------------------------------//
struct ExgMcMessageHandler {
   static void I010BasicInfoParser(ExgMcMessage& e) {
      auto& pk = *static_cast<const f9twf::ExgMcI010*>(&e.Pk_);
      auto  time = pk.InformationTime_.ToDayTime();
      auto* symbs = e.Channel_.ChannelMgr_->Symbs_.get();
      auto  locker = symbs->SymbMap_.Lock();
      auto& symb = *static_cast<ExgMdSymb*>(symbs->FetchSymb(locker, fon9::StrView_eos_or_all(pk.ProdId_.Chars_, ' ')).get());
      e.Symb_ = &symb;
      if (!symb.BasicInfoTime_.IsNull() && symb.BasicInfoTime_ >= time)
         return;
      symb.BasicInfoTime_ = time;
      symb.TradingMarket_ = (pk.TransmissionCode_ == '1' ? f9fmkt_TradingMarket_TwFUT : f9fmkt_TradingMarket_TwOPT);
      // symb.TradingSessionId_; 也許 ChannelMgr_ 增加 TradingSessionId_; 或可用 FlowGroup 來判斷?
      symb.FlowGroup_ = fon9::PackBcdTo<fon9::fmkt::SymbFlowGroup_t>(pk.FlowGroup_);
      symb.PriceOrigDiv_ = static_cast<uint32_t>(fon9::GetDecDivisor(
         static_cast<uint8_t>(fon9::fmkt::Pri::Scale - fon9::PackBcdTo<uint8_t>(pk.DecimalLocator_))));
      symb.StrikePriceDiv_ = static_cast<uint32_t>(fon9::GetDecDivisor(
         fon9::PackBcdTo<uint8_t>(pk.StrikePriceDecimalLocator_)));
      f9twf::ExgMdPriceTo(symb.Ref_.Data_.PriRef_,   pk.ReferencePrice_,  symb.PriceOrigDiv_);
      f9twf::ExgMdPriceTo(symb.Ref_.Data_.PriUpLmt_, pk.RiseLimitPrice1_, symb.PriceOrigDiv_);
      f9twf::ExgMdPriceTo(symb.Ref_.Data_.PriDnLmt_, pk.FallLimitPrice1_, symb.PriceOrigDiv_);
   }
   //-----------------------------------------------------------------------//
   static void I024MatchParser(ExgMcMessage&) {
      if (0);// 成交明細.
   }
   static void I081BSParser(ExgMcMessage& e) {
      auto& pk = *static_cast<const f9twf::ExgMcI081*>(&e.Pk_);
      auto  mdTime = e.Pk_.InformationTime_.ToDayTime();
      auto* symbs = e.Channel_.ChannelMgr_->Symbs_.get();
      auto  locker = symbs->SymbMap_.Lock();
      auto& symb = *static_cast<ExgMdSymb*>(symbs->FetchSymb(locker, fon9::StrView_eos_or_all(pk.ProdId_.Chars_, ' ')).get());
      e.Symb_ = &symb;
      ExgMdEntryToSymbBS(mdTime, fon9::PackBcdTo<unsigned>(pk.NoMdEntries_), pk.MdEntry_, symb);
   }
   static void I083BSParser(ExgMcMessage& e) {
      auto& pk = *static_cast<const f9twf::ExgMcI083*>(&e.Pk_);
      auto  mdTime = e.Pk_.InformationTime_.ToDayTime();
      auto* symbs = e.Channel_.ChannelMgr_->Symbs_.get();
      auto  locker = symbs->SymbMap_.Lock();
      auto& symb = *static_cast<ExgMdSymb*>(symbs->FetchSymb(locker, fon9::StrView_eos_or_all(pk.ProdId_.Chars_, ' ')).get());
      e.Symb_ = &symb;
      ExgMdEntryToSymbBS(mdTime, fon9::PackBcdTo<unsigned>(pk.NoMdEntries_), pk.MdEntry_, symb);
   }
   //-----------------------------------------------------------------------//
   static void I084SSParser(ExgMcMessage& e) {
      switch (static_cast<const f9twf::ExgMcI084*>(&e.Pk_)->MessageType_) {
      //case 'A': McI084SSParserA(e); break;
        case 'O': McI084SSParserO(e); break;
      //case 'S': McI084SSParserS(e); break;
      //case 'P': McI084SSParserP(e); break;
      //case 'Z': McI084SSParserZ(e); break;
      }
   }
   static void McI084SSParserO(ExgMcMessage& e) {
      auto&    pk = static_cast<const f9twf::ExgMcI084*>(&e.Pk_)->_O_OrderData_;
      auto     mdTime = e.Pk_.InformationTime_.ToDayTime();
      unsigned prodCount = fon9::PackBcdTo<unsigned>(pk.NoEntries_);
      auto*    prodEntry = pk.Entry_;
      auto*    symbs = e.Channel_.ChannelMgr_->Symbs_.get();
      auto     locker = symbs->SymbMap_.Lock();
      for (unsigned prodL = 0; prodL < prodCount; ++prodL) {
         auto& symb = *static_cast<ExgMdSymb*>(symbs->FetchSymb(locker, fon9::StrView_eos_or_all(prodEntry->ProdId_.Chars_, ' ')).get());
         prodEntry = static_cast<const f9twf::ExgMcI084::OrderDataEntry*>(
            ExgMdEntryToSymbBS(mdTime, fon9::PackBcdTo<unsigned>(prodEntry->NoMdEntries_), prodEntry->MdEntry_, symb));
      }
   }
};
//--------------------------------------------------------------------------//
// 即時行情接收流程: (必須區分 Fut or Opt)
// 1. 準備階段:
//    - 接收 [Channel 3.4.基本資料] 完整的一輪後, 可關閉 Receiver.
//      - 收完一輪之後, 才能進行下一階段.
//      - 因為要取得正確的 DECIMAL-LOCATOR;
//    - 此階段 Channel 1,2; 13,14; 都要先暫存不處理, 但仍要 [過濾重複] & [回補遺漏].
//      - Channel 13,14; 需要從 MESSAGE-TYPE == 'A': Refresh Begin 開始儲存.
//        - LAST-SEQ: 此輪 Refresh 訊息，對應即時行情傳輸群組之訊息流水序號。
//        - 待快照更新對應到即時行情訊息之末筆處理序號, 已銜接暫存即時行情序號。
//        - 參考快照更新對應之末筆處理序號，[捨棄小於等於] 末筆處理序號之相關暫存即時行情。
//        - 參考快照更新對應之末筆處理序號，[處理大於] 末筆處理序號之相關暫存即時行情。
//      - 需同時判斷 Channel 1,2; 是否可以接續? 如果不行, 就等下一輪再開始儲存[Channel 13,14].
//      - 收完一輪 Channel 13,14 後, 就可以關閉 Receiver.
// 2. 根據快照(Channel 13,14)更新商品委託簿.
//    - 從[即時行情暫存檔]載入&回放, 通知 Handler & Consumer.
// 3. 持續更新 Channel 1,2 即時行情.
//    - 若有行情漏失, 且已不允許回補, 要如何處理呢? 重啟 Channel 13,14 快照更新?
//    - 目前暫不支援此情況, 若有需要, 則需增加一個 Channel Receiver 註冊機制,
//      當需要收特定 Channel 的訊息時, 透過該機制通知 Receiver 啟動接收.
// ----------------------------------------------------------------------------
// 1. [即時行情 Channel] 預設為 Blocking 狀態.
//    即使在 Blocking 狀態, [即時行情 Channel] 依然必須確保連續、補漏.
// 2. [快照更新 Channel] 必須知道 [即時行情 Channel],
//    因為必須由 [快照更新 Channel] 來決定:
//    - [即時行情 Channel] 是否可以進入發行狀態.
//    - [即時行情 Channel] 要從哪個序號開始發行.
ExgMcChannelMgr::ExgMcChannelMgr(ExgMdSymbsSP symbs, fon9::StrView sysName, fon9::StrView groupName)
   : Name_{sysName.ToString() + "_" + groupName.ToString()}
   , Symbs_{std::move(symbs)} {
   // 即時行情.
   this->Channels_[ 1].Ctor(this,  1, ExgMcChannelStyle::PkLog | ExgMcChannelStyle::WaitSnapshot);
   this->Channels_[ 2].Ctor(this,  2, ExgMcChannelStyle::PkLog | ExgMcChannelStyle::WaitSnapshot);
   // 基本資料.
   this->Channels_[ 3].Ctor(this,  3, ExgMcChannelStyle::BasicInfo);
   this->Channels_[ 4].Ctor(this,  4, ExgMcChannelStyle::BasicInfo);
   // 文字公告. 盤中可能會有新增文字公告?
   this->Channels_[ 5].Ctor(this,  5, ExgMcChannelStyle::AnySeq);
   this->Channels_[ 6].Ctor(this,  6, ExgMcChannelStyle::AnySeq);
   // 統計資訊.
   this->Channels_[ 7].Ctor(this,  7, ExgMcChannelStyle::AnySeq);
   this->Channels_[ 8].Ctor(this,  8, ExgMcChannelStyle::AnySeq);
   // 詢價資訊.
   this->Channels_[ 9].Ctor(this,  9, ExgMcChannelStyle::AnySeq);
   this->Channels_[10].Ctor(this, 10, ExgMcChannelStyle::AnySeq);
   // 現貨資訊
   this->Channels_[11].Ctor(this, 11, ExgMcChannelStyle{});
   this->Channels_[12].Ctor(this, 12, ExgMcChannelStyle{});
   // 快照更新.
   this->Channels_[13].Ctor(this, 13, ExgMcChannelStyle::Snapshot);
   this->Channels_[14].Ctor(this, 14, ExgMcChannelStyle::Snapshot);
   // -----
   this->McDispatcher_.Reg('1', '1', 7, &ExgMcMessageHandler::I010BasicInfoParser);
   this->McDispatcher_.Reg('4', '1', 7, &ExgMcMessageHandler::I010BasicInfoParser);
   if (0);//契約基本資料: I011.KIND-ID, DECIMAL-LOCATOR, STRIKE-PRICE-DECIMAL-LOCATOR...
   // I011.END-SESSION 可以判斷 '0'一般交易時段, '1'盤後交易時段.
   //
   // 當 FetchSymb()->Contract_ == nullptr, 則應設定.
   // 複式商品的 DECIMAL-LOCATOR 也要從 Contract_ 取得.
   //
   // 盤後交易某些商品資訊必須待一般交易時段結束方可產生，盤後交易時段各商品開始傳送 I010 的時間點將不同。
   // => 所以不能用 Hb 來判斷是否 Cycled.

   this->McDispatcher_.Reg('2', 'A', 1, &ExgMcMessageHandler::I081BSParser);
   this->McDispatcher_.Reg('2', 'B', 1, &ExgMcMessageHandler::I083BSParser);
   this->McDispatcher_.Reg('2', 'C', 1, &ExgMcMessageHandler::I084SSParser);
   this->McDispatcher_.Reg('2', 'D', 1, &ExgMcMessageHandler::I024MatchParser);

   this->McDispatcher_.Reg('5', 'A', 1, &ExgMcMessageHandler::I081BSParser);
   this->McDispatcher_.Reg('5', 'B', 1, &ExgMcMessageHandler::I083BSParser);
   this->McDispatcher_.Reg('5', 'C', 1, &ExgMcMessageHandler::I084SSParser);
   this->McDispatcher_.Reg('5', 'D', 1, &ExgMcMessageHandler::I024MatchParser);
}
ExgMcChannelMgr::~ExgMcChannelMgr() {
}
void ExgMcChannelMgr::StartupChannelMgr(std::string logPath) {
   fon9_LOG_INFO(this->Name_, ".StartupChannelMgr|path=", logPath);
   for (ExgMcChannel& channel : this->Channels_)
      channel.StartupChannel(logPath);
   for (ExgMcChannel& channel : this->Channels_)
      channel.SetupReload();
}
void ExgMcChannelMgr::ChannelCycled(ExgMcChannel& src) {
   fon9_LOG_INFO(this->Name_, ".ChannelCycled|ChannelId=", src.GetChannelId());
   auto iMkt = (src.GetChannelId() % 2);
   if (src.IsBasicInfo()) {
      for (ExgMcChannel& c : this->Channels_) {
         if (c.GetChannelId() % 2 == iMkt)
            c.OnBasicInfoCycled();
      }
      ExgMcChannel& ss = GetSnapshotChannel(src);
      if (ss.GetChannelState() == ExgMcChannelState::CanBeClosed)
         this->OnSnapshotDone(ss, ss.CycleStartSeq_);
   }
}
void ExgMcChannelMgr::OnSnapshotDone(ExgMcChannel& src, uint64_t lastRtSeq) {
   assert(src.IsSnapshot());
   fon9_LOG_INFO(this->Name_, ".SnapshotDone|ChannelId=", src.GetChannelId());
   auto iMkt = (src.GetChannelId() % 2);
   for (ExgMcChannel& c : this->Channels_) {
      if (c.GetChannelId() % 2 == iMkt)
         c.OnSnapshotDone(lastRtSeq);
   }
}

} // namespaces
