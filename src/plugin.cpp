#include "plugin.h"

#ifdef _WIN32
#include "gui.h"
#else
#include "gui_gtk.h"
#endif

#include <clap/helpers/host-proxy.hxx>
#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <cstring>
#include <memory>

namespace ytb {

static const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                        CLAP_PLUGIN_FEATURE_UTILITY, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "dev.ytaudiobrowser.clap",
    "YT Audio Browser",
    "YtAudioBrowser",
    "https://github.com/yt-dlp/yt-dlp",
    "",
    "",
    "1.0.0",
    "Search several sites with yt-dlp, download audio, drag it into your DAW.",
    kFeatures};

namespace {

using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Ignore,
                                   clap::helpers::CheckingLevel::Minimal>;

// Audio passes through untouched. The plugin exists for its window.
class YtAudioBrowser final : public Base {
 public:
   explicit YtAudioBrowser(const clap_host *host) : Base(&kDescriptor, host) {}

   bool implementsAudioPorts() const noexcept override { return true; }
   uint32_t audioPortsCount(bool) const noexcept override { return 1; }
   bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info *info) const noexcept override {
      if (index != 0)
         return false;
      info->id = isInput ? 0 : 1;
      std::strncpy(info->name, "Main", sizeof(info->name));
      info->flags = CLAP_AUDIO_PORT_IS_MAIN;
      info->channel_count = 2;
      info->port_type = CLAP_PORT_STEREO;
      info->in_place_pair = CLAP_INVALID_ID;
      return true;
   }

   clap_process_status process(const clap_process *p) noexcept override {
      if (p->audio_inputs_count == 0 || p->audio_outputs_count == 0)
         return CLAP_PROCESS_CONTINUE;
      const clap_audio_buffer_t &in = p->audio_inputs[0];
      clap_audio_buffer_t &out = p->audio_outputs[0];
      const uint32_t channels = in.channel_count < out.channel_count ? in.channel_count : out.channel_count;
      for (uint32_t c = 0; c < channels; ++c) {
         if (in.data32 && out.data32) {
            if (in.data32[c] != out.data32[c])
               std::memcpy(out.data32[c], in.data32[c], p->frames_count * sizeof(float));
         } else if (in.data64 && out.data64) {
            if (in.data64[c] != out.data64[c])
               std::memcpy(out.data64[c], in.data64[c], p->frames_count * sizeof(double));
         }
      }
      return CLAP_PROCESS_CONTINUE;
   }

#ifdef _WIN32
   // ---- Windows: embedded child window --------------------------------------
   bool implementsGui() const noexcept override { return true; }
   bool guiIsApiSupported(const char *api, bool isFloating) noexcept override {
      return !isFloating && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
   }
   bool guiGetPreferredApi(const char **api, bool *isFloating) noexcept override {
      *api = CLAP_WINDOW_API_WIN32;
      *isFloating = false;
      return true;
   }
   bool guiCreate(const char *api, bool isFloating) noexcept override {
      if (!guiIsApiSupported(api, isFloating))
         return false;
      _gui = std::make_unique<Gui>();
      return true;
   }
   void guiDestroy() noexcept override { _gui.reset(); }
   bool guiSetScale(double) noexcept override { return false; }
   bool guiGetSize(uint32_t *w, uint32_t *h) noexcept override {
      if (!_gui)
         return false;
      _gui->size(*w, *h);
      return true;
   }
   bool guiCanResize() const noexcept override { return false; }
   bool guiSetParent(const clap_window *window) noexcept override {
      return _gui && window && _gui->attach(static_cast<HWND>(window->win32));
   }
   bool guiShow() noexcept override {
      if (!_gui)
         return false;
      _gui->show();
      return true;
   }
   bool guiHide() noexcept override {
      if (!_gui)
         return false;
      _gui->hide();
      return true;
   }

 private:
   std::unique_ptr<Gui> _gui;
#else
   // ---- Linux: floating GTK window, pumped from the host's timer ------------
   // GTK cannot embed into a host-provided X11 window, so the window floats and
   // the host marks it as belonging to its own window (set_transient).
   bool implementsGui() const noexcept override { return true; }
   bool guiIsApiSupported(const char *api, bool isFloating) noexcept override {
      return isFloating && std::strcmp(api, CLAP_WINDOW_API_X11) == 0;
   }
   bool guiGetPreferredApi(const char **api, bool *isFloating) noexcept override {
      *api = CLAP_WINDOW_API_X11;
      *isFloating = true;
      return true;
   }
   bool guiCreate(const char *api, bool isFloating) noexcept override {
      if (!guiIsApiSupported(api, isFloating) || !_host.canUseTimerSupport())
         return false;
      _gui = std::make_unique<GtkGui>([this] {
         // The user closed the window with the window manager's button.
         if (_host.canUseGui())
            _host.guiClosed(false);
      });
      if (!_gui->create()) {
         _gui.reset();
         return false;
      }
      if (!_host.timerSupportRegister(20, &_timer)) {
         _gui.reset();
         return false;
      }
      return true;
   }
   void guiDestroy() noexcept override {
      if (_timer != CLAP_INVALID_ID) {
         _host.timerSupportUnregister(_timer);
         _timer = CLAP_INVALID_ID;
      }
      _gui.reset();
   }
   bool guiSetScale(double) noexcept override { return false; }
   bool guiCanResize() const noexcept override { return false; }
   bool guiSetTransient(const clap_window *window) noexcept override {
      return _gui && window && _gui->setTransient(static_cast<unsigned long>(window->x11));
   }
   void guiSuggestTitle(const char *title) noexcept override {
      if (_gui && title)
         _gui->setTitle(title);
   }
   bool guiShow() noexcept override {
      if (!_gui)
         return false;
      _gui->show();
      return true;
   }
   bool guiHide() noexcept override {
      if (!_gui)
         return false;
      _gui->hide();
      return true;
   }
   bool implementsTimerSupport() const noexcept override { return true; }
   void onTimer(clap_id id) noexcept override {
      if (id == _timer && _gui)
         _gui->pump();
   }

 private:
   std::unique_ptr<GtkGui> _gui;
   clap_id _timer = CLAP_INVALID_ID;
#endif
};

} // namespace

const clap_plugin_t *createPlugin(const clap_host_t *host) {
   return (new YtAudioBrowser(host))->clapPlugin();
}

} // namespace ytb
