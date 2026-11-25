#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>

namespace ytb {

// Embedded Win32 view: search box, site toggles, results list, download
// controls and a list of downloaded files that can be dragged into the DAW.
class Gui {
 public:
   Gui();
   ~Gui();

   bool attach(HWND parent);
   void show();
   void hide();
   void size(uint32_t &w, uint32_t &h) const;

   static void unregisterWindowClass();

 private:
   struct Impl;
   std::unique_ptr<Impl> d;
};

} // namespace ytb
