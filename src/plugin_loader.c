#include "kworker_internal.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_so_suffix(const char *name)
{
    size_t length = strlen(name);
    return length > 3 && strcmp(name + length - 3, ".so") == 0;
}

static int compare_plugin(const void *left, const void *right)
{
    const struct kw_loaded_plugin *a = left;
    const struct kw_loaded_plugin *b = right;
    if (a->api->priority < b->api->priority)
        return -1;
    if (a->api->priority > b->api->priority)
        return 1;
    return strcmp(a->api->name, b->api->name);
}

static void unload_all(struct kw_runtime *runtime)
{
    size_t i = runtime->plugin_count;
    while (i > 0) {
        struct kw_loaded_plugin *plugin = &runtime->plugins[--i];
        if (plugin->started && plugin->api->stop)
            plugin->api->stop();
        plugin->started = 0;
    }
    i = runtime->plugin_count;
    while (i > 0) {
        struct kw_loaded_plugin *plugin = &runtime->plugins[--i];
        if (plugin->initialized && plugin->api->destroy)
            plugin->api->destroy();
        plugin->initialized = 0;
        if (plugin->handle)
            dlclose(plugin->handle);
        free(plugin->path);
        memset(plugin, 0, sizeof(*plugin));
    }
    runtime->plugin_count = 0;
    memset(runtime->services, 0, sizeof(runtime->services));
    runtime->service_count = 0;
}

static int inspect_one(struct kw_runtime *runtime, const char *path)
{
    struct kw_loaded_plugin *slot;
    kw_plugin_entry_fn entry;
    const struct kw_plugin_v2 *api;
    const char *error;

    if (runtime->plugin_count == KW_MAX_PLUGINS) {
        fprintf(stderr, "Too many plugins; maximum is %d.\n", KW_MAX_PLUGINS);
        return 0;
    }
    slot = &runtime->plugins[runtime->plugin_count];
    slot->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!slot->handle) {
        fprintf(stderr, "Cannot load %s: %s\n", path, dlerror());
        return 0;
    }
    dlerror();
    *(void **)(&entry) = dlsym(slot->handle, KW_PLUGIN_ENTRY_SYMBOL);
    error = dlerror();
    if (error || !entry) {
        fprintf(stderr, "%s has no %s: %s\n", path, KW_PLUGIN_ENTRY_SYMBOL,
                error ? error : "null entry point");
        dlclose(slot->handle);
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    api = entry();
    if (!api || api->abi_version != KW_PLUGIN_ABI_V2 ||
        api->struct_size < sizeof(*api) || !api->name || !*api->name || !api->init) {
        fprintf(stderr, "%s exposes an incompatible plugin descriptor.\n", path);
        dlclose(slot->handle);
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    slot->api = api;
    slot->path = strdup(path);
    if (!slot->path) {
        dlclose(slot->handle);
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    runtime->plugin_count++;
    return 1;
}

int kw_plugins_load(struct kw_runtime *runtime)
{
    DIR *directory;
    struct dirent *entry;
    int ok = 1;

    directory = opendir(runtime->config.plugin_dir);
    if (!directory) {
        fprintf(stderr, "Cannot open plugin directory %s: %s\n",
                runtime->config.plugin_dir, strerror(errno));
        return 0;
    }
    while ((entry = readdir(directory)) != NULL) {
        char *path;
        size_t needed;
        if (entry->d_name[0] == '.' || !has_so_suffix(entry->d_name))
            continue;
        needed = strlen(runtime->config.plugin_dir) + strlen(entry->d_name) + 2;
        path = malloc(needed);
        if (!path) {
            ok = 0;
            break;
        }
        snprintf(path, needed, "%s/%s", runtime->config.plugin_dir, entry->d_name);
        if (!inspect_one(runtime, path))
            ok = 0;
        free(path);
        if (!ok)
            break;
    }
    closedir(directory);
    if (!ok) {
        unload_all(runtime);
        return 0;
    }
    qsort(runtime->plugins, runtime->plugin_count,
          sizeof(runtime->plugins[0]), compare_plugin);
    for (size_t i = 0; i < runtime->plugin_count; ++i) {
        for (size_t j = i + 1; j < runtime->plugin_count; ++j) {
            if (strcmp(runtime->plugins[i].api->name,
                       runtime->plugins[j].api->name) == 0) {
                fprintf(stderr, "Duplicate plugin name %s.\n",
                        runtime->plugins[i].api->name);
                unload_all(runtime);
                return 0;
            }
        }
        if (!runtime->plugins[i].api->init(&runtime->host_api)) {
            fprintf(stderr, "Plugin %s failed initialization.\n",
                    runtime->plugins[i].api->name);
            unload_all(runtime);
            return 0;
        }
        runtime->plugins[i].initialized = 1;
    }
    for (size_t i = 0; i < runtime->plugin_count; ++i) {
        if (runtime->plugins[i].api->start && !runtime->plugins[i].api->start()) {
            fprintf(stderr, "Plugin %s failed startup.\n",
                    runtime->plugins[i].api->name);
            unload_all(runtime);
            return 0;
        }
        runtime->plugins[i].started = 1;
        kw_log(runtime, KW_LOG_INFO, runtime->plugins[i].api->name, "loaded");
    }
    return 1;
}

void kw_plugins_stop(struct kw_runtime *runtime)
{
    unload_all(runtime);
}

int kw_plugins_reload(struct kw_runtime *runtime)
{
    kw_log(runtime, KW_LOG_INFO, "host", "reloading plugins");
    unload_all(runtime);
    return kw_plugins_load(runtime);
}

void kw_plugins_event(struct kw_runtime *runtime, const struct kw_ao_event *event)
{
    for (size_t i = 0; i < runtime->plugin_count; ++i)
        if (runtime->plugins[i].api->on_event)
            runtime->plugins[i].api->on_event(event);
}

void kw_plugins_tick(struct kw_runtime *runtime)
{
    for (size_t i = 0; i < runtime->plugin_count; ++i)
        if (runtime->plugins[i].api->on_tick)
            runtime->plugins[i].api->on_tick();
}
