#ifndef KWORKER_PLUGIN_H
#define KWORKER_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#define KW_PLUGIN_ABI_V2 2u
#define KW_MAX_WORKERS 32u
#define KW_PLUGIN_ENTRY_SYMBOL "kworker_plugin_v2"

enum kw_log_level { KW_LOG_DEBUG, KW_LOG_INFO, KW_LOG_WARN, KW_LOG_ERROR };

enum kw_ao_event_type {
    KW_AO_ONLINE,
    KW_AO_BUDDY_STATUS,
    KW_AO_BUDDY_REMOVED,
    KW_AO_PRIVATE_MESSAGE,
    KW_AO_CLIENT_NAME,
    KW_AO_LOOKUP_RESULT
};

struct kw_ao_event {
    enum kw_ao_event_type type;
    size_t worker_index;
    uint32_t uid;
    int online;
    const char *text;
    size_t text_len;
};

struct kw_host_v2 {
    uint32_t abi_version;
    size_t struct_size;
    void *context;
    void (*log)(void *context, enum kw_log_level level,
                const char *plugin, const char *message);
    int (*register_service)(void *context, const char *name,
                            uint32_t version, const void *service);
    const void *(*find_service)(void *context, const char *name,
                                uint32_t version);
    int (*ao_buddy_add)(void *context, size_t worker_index, uint32_t uid,
                        const char *tag, size_t tag_len);
    int (*ao_buddy_remove)(void *context, size_t worker_index, uint32_t uid);
    int (*ao_tell)(void *context, size_t worker_index, uint32_t uid,
                   const char *message, size_t message_len);
    size_t (*worker_count)(void *context);
    const char *(*worker_name)(void *context, size_t worker_index);
    uint32_t (*worker_uid)(void *context, size_t worker_index);
    int (*worker_online)(void *context, size_t worker_index);
};

struct kw_plugin_v2 {
    uint32_t abi_version;
    size_t struct_size;
    const char *name;
    int priority;
    int (*init)(const struct kw_host_v2 *host);
    int (*start)(void);
    void (*on_event)(const struct kw_ao_event *event);
    void (*on_tick)(void);
    void (*stop)(void);
    void (*destroy)(void);
};

typedef const struct kw_plugin_v2 *(*kw_plugin_entry_fn)(void);

#endif
