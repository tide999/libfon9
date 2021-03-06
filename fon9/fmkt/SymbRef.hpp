﻿// \file fon9/fmkt/SymbRef.hpp
// \author fonwinz@gmail.com
#ifndef __fon9_fmkt_SymbRef_hpp__
#define __fon9_fmkt_SymbRef_hpp__
#include "fon9/fmkt/SymbDy.hpp"
#include "fon9/fmkt/FmktTypes.hpp"

namespace fon9 { namespace fmkt {

/// \ingroup fmkt
/// 商品資料的擴充: 參考價.
class fon9_API SymbRef : public SymbData {
   fon9_NON_COPY_NON_MOVE(SymbRef);
public:
   struct Data {
      Pri   PriRef_{};
      Pri   PriUpLmt_{};
      Pri   PriDnLmt_{};
   };
   Data  Data_;

   SymbRef(const Data& rhs) : Data_(rhs) {
   }
   SymbRef() = default;

   void DailyClear() {
      memset(&this->Data_, 0, sizeof(this->Data_));
   }

   static seed::Fields MakeFields();
};

class fon9_API SymbRefTabDy : public SymbDataTab {
   fon9_NON_COPY_NON_MOVE(SymbRefTabDy);
   using base = SymbDataTab;
public:
   SymbRefTabDy(Named&& named)
      : base{std::move(named), SymbRef::MakeFields(), seed::TabFlag::NoSapling_NoSeedCommand_Writable} {
   }

   SymbDataSP FetchSymbData(Symb&) override;
};

} } // namespaces
#endif//__fon9_fmkt_SymbRef_hpp__
