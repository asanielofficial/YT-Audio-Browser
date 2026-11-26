#include "plugin.h"
#ifdef _WIN32
#include "gui.h"
#endif

#include <clap/clap.h>

#include <cstring>

namespace {

uint32_t factoryCount(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *factoryDescriptor(const clap_plugin_factory_t *, uint32_t index) {
   return index == 0 ? &ytb::kDescriptor : nullptr;
}

const clap_plugin_t *factoryCreate(const clap_plugin_factory_t *, const clap_host_t *host,
                                   const char *pluginId) {
   if (!host || !pluginId || std::strcmp(pluginId, ytb::kDescriptor.id) != 0)
      return nullptr;
   if (!clap_version_is_compatible(host->clap_version))
      return nullptr;
   return ytb::createPlugin(host);
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char *) { return true; }
void entryDeinit() {
#ifdef _WIN32
   ytb::Gui::unregisterWindowClass();
#endif
}

const void *entryGetFactory(const char *id) {
   return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" {
CLAP_EXPORT extern const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit,
                                                          entryDeinit, entryGetFactory};
}
