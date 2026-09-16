#ifndef KWORKER_INTERNAL_H
#define KWORKER_INTERNAL_H

#include "kworker_plugin.h"

#include <signal.h>
#include <stdint.h>

#define KW_MAX_PLUGINS 32
#define KW_MAX_SERVICES 32
#define KW_LOGIN_TIMEOUT 60
#define KW_PING_INTERVAL 60
#define KW_CONFIG_TEXT 1025
#define KW_PATH_TEXT 4096
#define KW_HOST_TEXT 256

struct kw_account {
    char username[KW_CONFIG_TEXT];
    char password[KW_CONFIG_TEXT];
    char character[16];
};

struct kw_config {
    struct kw_account workers[KW_MAX_WORKERS];
    size_t worker_count;
    char plugin_dir[KW_PATH_TEXT];
    char chat_host[KW_HOST_TEXT];
    int chat_port;
};

struct kw_service_slot {
    char name[64];
    uint32_t version;
    const void *service;
};

struct kw_loaded_plugin {
    void *handle;
    const struct kw_plugin_v2 *api;
    char *path;
    int initialized;
    int started;
};

struct kw_worker_state {
    void *ao_connection;
    uint32_t uid;
    int online;
    int stage;
    long login_deadline;
    long next_ping;
};

struct kw_runtime {
    struct kw_config config;
    struct kw_host_v2 host_api;
    struct kw_service_slot services[KW_MAX_SERVICES];
    size_t service_count;
    struct kw_loaded_plugin plugins[KW_MAX_PLUGINS];
    size_t plugin_count;
    struct kw_worker_state workers[KW_MAX_WORKERS];
};

extern volatile sig_atomic_t kw_stop_requested;
extern volatile sig_atomic_t kw_reload_requested;

int kw_config_load(struct kw_config *config);
int kw_plugins_load(struct kw_runtime *runtime);
void kw_plugins_stop(struct kw_runtime *runtime);
int kw_plugins_reload(struct kw_runtime *runtime);
void kw_plugins_event(struct kw_runtime *runtime, const struct kw_ao_event *event);
void kw_plugins_tick(struct kw_runtime *runtime);
void kw_log(void *context, enum kw_log_level level,
            const char *plugin, const char *message);

#endif

