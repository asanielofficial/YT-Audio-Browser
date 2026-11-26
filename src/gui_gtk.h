#pragma once

// Linux window: a floating GTK3 window. The header has no GTK types so the
// rest of the plugin does not need GTK's headers.

#include <functional>
#include <memory>

namespace ytb {

class GtkGui {
 public:
   // `onClosed` runs on the host's main thread when the user closes the window.
   explicit GtkGui(std::function<void()> onClosed);
   ~GtkGui();

   bool create();                       // builds the (hidden) window; false if no display
   bool setTransient(unsigned long x11Window); // keep above the host's window
   void setTitle(const char *title);
   void show();
   void hide();
   void pump(); // runs pending GTK events; call from the host's timer

 private:
   struct Impl;
   std::unique_ptr<Impl> d;
};

} // namespace ytb
