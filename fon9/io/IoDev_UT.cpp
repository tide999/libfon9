﻿/// \file fon9/io/IoDev_UT.cpp
/// \author fonwinz@gmail.com
#include "fon9/io/SimpleManager.hpp"
#include "fon9/TestTools.hpp"

#ifdef fon9_WINDOWS
#include "fon9/io/win/IocpTcpClient.hpp"
using TcpClient = fon9::io::IocpTcpClient;
using IoService = fon9::io::IocpService;
#else
#include "fon9/io/FdrTcpClient.hpp"
#include "fon9/io/FdrServiceEpoll.hpp"
using TcpClient = fon9::io::FdrTcpClient;
using IoService = fon9::io::FdrServiceEpoll;
#endif

using TimeUS = fon9::Decimal<uint64_t, 3>;
TimeUS GetTimeUS() {
   using namespace std::chrono;
   return TimeUS::Make<3>(static_cast<uint64_t>(duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count()));
}

//--------------------------------------------------------------------------//

fon9_WARN_DISABLE_PADDING;
class PingpongSession : public fon9::io::Session {
   fon9_NON_COPY_NON_MOVE(PingpongSession);
   bool IsEchoMode_;

   virtual fon9::io::RecvBufferSize OnDevice_LinkReady(fon9::io::Device&) override {
      return fon9::io::RecvBufferSize::Default;
   }
   virtual fon9::io::RecvBufferSize OnDevice_Recv(fon9::io::Device& dev, fon9::DcQueueList& rxbuf) override {
      this->LastRecvTime_ = fon9::UtcNow();
      size_t rxsz = fon9::CalcDataSize(rxbuf.cfront());
      this->RecvBytes_ += rxsz;
      ++this->RecvCount_;
      if (this->IsEchoMode_)
         dev.Send(rxbuf.MoveOut());
      else
         rxbuf.MoveOut();
      return fon9::io::RecvBufferSize::Default;
   }
   virtual std::string SessionCommand(fon9::io::Device& dev, fon9::StrView cmdln) override {
      // cmdln = "a size" or "b size" or "s string"
      // a = ASAP, b = Buffered
      // size = 1024 or 1024k or 100m ...
      //        k=*1000, m=*1000000
      TimeUS         usElapsed;
      size_t         size;
      fon9::StrView  strSend;
      std::string    retval;
      fon9::io::Device::SendResult res;
__NEXT_CMD:
      switch (int chCmd = fon9::StrTrim(&cmdln).Get1st()) {
      default:
         return retval.empty() ? std::string{"Unknown ses command."} : retval;
      case 'e':
         this->IsEchoMode_ = true;
         cmdln.SetBegin(cmdln.begin() + 1);
         retval = "echo on";
         goto __NEXT_CMD;
      case 's':
         size = cmdln.size();
         strSend = "Send";
         this->LastSendTime_ = fon9::UtcNow();
         usElapsed = GetTimeUS();
         res = dev.StrSend(cmdln);
         break;
      case 'a': case 'b':
         cmdln.SetBegin(cmdln.begin() + 1);
         const char* endp;
         size = fon9::StrTo(cmdln, 0u, &endp);
         if (size <= 0)
            return "tx size empty.";
         cmdln.SetBegin(endp);
         switch(fon9::StrTrimHead(&cmdln).Get1st()) {
         case 'm':   size *= 1000;
         case 'k':   size *= 1000;  break;
         }
         std::string msg(size, '\0');
         this->LastSendTime_ = fon9::UtcNow();
         usElapsed = GetTimeUS();
         if (chCmd == 'a') {
            strSend = "SendASAP";
            res = dev.SendASAP(msg.data(), msg.size());
         }
         else {
            strSend = "SendBuffered";
            res = dev.SendBuffered(msg.data(), msg.size());
         }
         break;
      }
      usElapsed = GetTimeUS() - usElapsed;
      fon9_LOG_DEBUG("dev.", strSend, "()"
                     "|res=", res,
                     "|size=", size, fon9::FmtDef{","},
                     "|elapsed=", usElapsed, fon9::FmtDef{","}, "(us)");
      return std::string{};
   }
public:
   PingpongSession() = default;

   fon9::TimeStamp   LastSendTime_;
   fon9::TimeStamp   LastRecvTime_;
   uint64_t          RecvBytes_{0};
   uint64_t          RecvCount_{0};

   void PrintInfo() {
      if (this->RecvCount_ <= 0)
         return;
      if (this->IsEchoMode_) {
         this->IsEchoMode_ = false;
         std::this_thread::sleep_for(std::chrono::milliseconds{500});
      }
      fon9::TimeInterval elapsed = (this->LastRecvTime_ - this->LastSendTime_);
      fon9_LOG_INFO("Session info"
                    "|lastRecvTime=", this->LastRecvTime_,
                    "|lastSendTime=", this->LastSendTime_,
                    "|elapsed=", elapsed,
                    "\n|recvBytes=", this->RecvBytes_,
                    "|recvCount=", this->RecvCount_,
                    "|avgBytes=", this->RecvBytes_ / this->RecvCount_,
                    "|throughput=", fon9::Decimal<uint64_t,3>(this->RecvBytes_ / (elapsed.To<double>() * 1024 * 1024)),
                    "(MB/s)");
      this->RecvBytes_ = this->RecvCount_ = 0;
   }
};
using PingpongSP = fon9::intrusive_ptr<PingpongSession>;
fon9_WARN_POP;

//--------------------------------------------------------------------------//

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
   _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
   fon9::AutoPrintTestInfo utinfo("IoDev");

   IoService::MakeResult err;
   auto iosv{IoService::MakeService(fon9::io::IoServiceArgs{}, "IoTest", err)};
   if (!iosv) {
      std::cout << "IoService.MakeService()|" << fon9::RevPrintTo<std::string>(err) << std::endl;
      return 0;
   }

   fon9::io::ManagerSP mgr{new fon9::io::SimpleManager{}};
   PingpongSP          ses{new PingpongSession{}};
   fon9::io::DeviceSP  dev{new TcpClient(iosv, ses, mgr)};
#ifdef fon9_WINDOWS
   dev->AsyncOpen("192.168.1.16:6000|Timeout=30|SNDBUF=0");
#else
   dev->AsyncOpen("192.168.1.16:6000|Timeout=30");
#endif

   char cmdbuf[1024];
   while (fgets(cmdbuf, sizeof(cmdbuf), stdin)) {
      fon9::StrView cmd{fon9::StrView_cstr(cmdbuf)};
      fon9::StrTrim(&cmd);
      if (cmd.empty()) {
         ses->PrintInfo();
         continue;
      }
      if (cmd == "quit")
         break;
      auto dres = dev->DeviceCommand(cmd);
      if (!dres.empty())
         std::cout << dres << std::endl;
   }
   dev->AsyncDispose("quit");
}
