﻿/// \file fon9/io/TcpClientBase.hpp
/// \author fonwinz@gmail.com
#ifndef __fon9_io_TcpClientBase_hpp__
#define __fon9_io_TcpClientBase_hpp__
#include "fon9/io/Device.hpp"
#include "fon9/io/Socket.hpp"
#include "fon9/io/SocketClient.hpp"
#include "fon9/io/SocketAddressDN.hpp"
#include "fon9/Timer.hpp"

namespace fon9 { namespace io {

/// \ingroup io
/// TcpClient 基底, 衍生者: IocpTcpClient, FdrTcpClient;
/// - 查找設定裡面的 DN: Config_.DomainNames_
/// - 連線時依序嘗試 AddrList_ 裡面的位置 & port, 全都失敗才觸發 LinkError.
class fon9_API TcpClientBase : public Device {
   fon9_NON_COPY_NON_MOVE(TcpClientBase);
   TcpClientBase() = delete;
   using base = Device;

   template<class DeviceT>
   friend bool OpThr_ParseDeviceConfig(DeviceT& dev, StrView cfgstr, FnOnTagValue fnUnknownField);

protected:
   SocketClientConfig   Config_;
   size_t               NextAddrIndex_;
   SocketAddressList    AddrList_;
   DnQueryReqId         DnReqId_{};
   SocketAddress        RemoteAddress_;

   static void OnConnectTimeout(TimerEntry* timer, TimeStamp now);
   using Timer = DataMemberEmitOnTimer<&TcpClientBase::OnConnectTimeout>;
   Timer ConnectTimer_;
   void StartConnectTimer() {
      this->ConnectTimer_.RunAfter(TimeInterval_Second(this->Config_.TimeoutSecs_));
   }

   void OpThr_ConnectToNext(StrView lastError);
   void OpThr_ReopenImpl();
   void OpThr_OnDnQueryDone(DnQueryReqId id, const DomainNameParseResult& res);
   void OpThr_Connected(const Socket& soCli);
   virtual bool OpThr_ConnectToImpl(Socket&& soCli, SocketResult& soRes) = 0;
   /// 用來清除「用來處理連線」的資源.
   virtual void OpThr_TcpClearLinking();
   /// 用來清除緩衝區、Socket資源...
   virtual void OpThr_TcpLinkBroken() = 0;

   virtual void OpImpl_Open(std::string cfgstr) override;
   virtual void OpImpl_Reopen() override;
   virtual void OpImpl_Close(std::string cause) override;
   virtual void OpImpl_StateChanged(const StateChangedArgs& e) override;

public:
   TcpClientBase(SessionSP ses, ManagerSP mgr)
      : base(std::move(ses), std::move(mgr), Style::Client)
      , ConnectTimer_{GetDefaultTimerThread()} {
   }
   ~TcpClientBase();
};

} } // namespaces
#endif//__fon9_io_TcpClientBase_hpp__
