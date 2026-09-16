#ifndef KWORKER_SERVICES_H
#define KWORKER_SERVICES_H

#include <stddef.h>
#include <stdint.h>

#define KW_SERVICE_V2 2u
#define KW_BUDDY_SERVICE "kworker.buddy"
#define KW_TELL_SERVICE "kworker.tell"

typedef void (*kw_buddy_sink_fn)(void *context, size_t worker_index,
                                 uint32_t uid, int online, int removed);
typedef void (*kw_tell_sink_fn)(void *context, size_t worker_index,
                                uint32_t uid, const char *message,
                                size_t message_len);

struct kw_buddy_service_v2 {
    /* add/remove: 1 accepted, 0 invalid/allocation failure, -1 retry later.
     * Producers must retain the request on -1; never wait on the AO thread. */
    int (*add)(size_t worker_index, uint32_t uid,
               const char *tag, size_t tag_len);
    int (*remove)(size_t worker_index, uint32_t uid);
    int (*set_sink)(kw_buddy_sink_fn sink, void *context);
    size_t (*count)(size_t worker_index);
};

struct kw_tell_service_v2 {
    int (*enqueue)(size_t worker_index, uint32_t uid,
                   const char *message, size_t message_len);
    int (*set_sink)(kw_tell_sink_fn sink, void *context);
    size_t (*queued)(size_t worker_index);
};

#endif
