#include "kworker_plugin.h"
#include "kworker_services.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BUDDY_QUEUE_LIMIT (4096u * KW_MAX_WORKERS)
#define BUDDY_SET_LIMIT 2048

enum buddy_operation { BUDDY_ADD, BUDDY_REMOVE };

struct buddy_request {
    enum buddy_operation operation;
    size_t worker_index;
    uint32_t uid;
    char *tag;
    size_t tag_len;
    struct buddy_request *next;
};

static const struct kw_host_v2 *host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct buddy_request *head[KW_MAX_WORKERS], *tail[KW_MAX_WORKERS];
static struct timespec next_send[KW_MAX_WORKERS];
static size_t queue_size;
static size_t buddy_count[KW_MAX_WORKERS];
static uint32_t buddy_ids[KW_MAX_WORKERS][BUDDY_SET_LIMIT];
static kw_buddy_sink_fn event_sink;
static void *event_sink_context;

static int enqueue(enum buddy_operation operation, size_t worker_index,
                   uint32_t uid, const char *tag, size_t tag_len)
{
    struct buddy_request *request;
    if (!host || worker_index >= host->worker_count(host->context) ||
        !uid || tag_len > 65535 || (tag_len && !tag))
        return 0;
    request = calloc(1, sizeof(*request));
    if (!request)
        return 0;
    if (tag_len) {
        request->tag = malloc(tag_len);
        if (!request->tag) {
            free(request);
            return 0;
        }
        memcpy(request->tag, tag, tag_len);
    }
    request->operation = operation;
    request->worker_index = worker_index;
    request->uid = uid;
    request->tag_len = tag_len;

    pthread_mutex_lock(&lock);
    /* Collapse repeated identical pending requests, but preserve add/remove
     * transitions (Nadybot waits for removal acknowledgements to rebalance). */
    struct buddy_request *last = NULL;
    for (struct buddy_request *p = head[worker_index]; p; p = p->next)
        if (p->uid == uid)
            last = p;
    if (last && last->operation == operation && last->tag_len == tag_len &&
        (!tag_len || memcmp(last->tag, tag, tag_len) == 0)) {
        pthread_mutex_unlock(&lock);
        free(request->tag);
        free(request);
        return 1;
    }
    if (queue_size == BUDDY_QUEUE_LIMIT) {
        pthread_mutex_unlock(&lock);
        free(request->tag);
        free(request);
        return -1; /* temporary backpressure, not a dropped operation */
    }
    if (tail[worker_index])
        tail[worker_index]->next = request;
    else
        head[worker_index] = request;
    tail[worker_index] = request;
    queue_size++;
    pthread_mutex_unlock(&lock);
    return 1;
}

static int add_buddy(size_t worker_index, uint32_t uid,
                     const char *tag, size_t tag_len)
{
    return enqueue(BUDDY_ADD, worker_index, uid, tag, tag_len);
}

static int remove_buddy(size_t worker_index, uint32_t uid)
{
    return enqueue(BUDDY_REMOVE, worker_index, uid, NULL, 0);
}

static int set_sink(kw_buddy_sink_fn sink, void *context)
{
    pthread_mutex_lock(&lock);
    event_sink = sink;
    event_sink_context = context;
    pthread_mutex_unlock(&lock);
    return 1;
}

static size_t count_buddies(size_t worker_index)
{
    size_t result = 0;
    pthread_mutex_lock(&lock);
    if (worker_index < KW_MAX_WORKERS)
        result = buddy_count[worker_index];
    pthread_mutex_unlock(&lock);
    return result;
}

static void remember_buddy(size_t worker_index, uint32_t uid)
{
    if (worker_index >= KW_MAX_WORKERS)
        return;
    for (size_t i = 0; i < buddy_count[worker_index]; ++i)
        if (buddy_ids[worker_index][i] == uid)
            return;
    if (buddy_count[worker_index] < BUDDY_SET_LIMIT)
        buddy_ids[worker_index][buddy_count[worker_index]++] = uid;
}

static void forget_buddy(size_t worker_index, uint32_t uid)
{
    if (worker_index >= KW_MAX_WORKERS)
        return;
    for (size_t i = 0; i < buddy_count[worker_index]; ++i) {
        if (buddy_ids[worker_index][i] != uid)
            continue;
        buddy_ids[worker_index][i] =
            buddy_ids[worker_index][buddy_count[worker_index] - 1];
        buddy_count[worker_index]--;
        return;
    }
}

static const struct kw_buddy_service_v2 service = {
    add_buddy, remove_buddy, set_sink, count_buddies
};

static int plugin_init(const struct kw_host_v2 *host_api)
{
    if (!host_api || host_api->abi_version != KW_PLUGIN_ABI_V2 ||
        host_api->struct_size < sizeof(*host_api))
        return 0;
    host = host_api;
    return host->register_service(host->context, KW_BUDDY_SERVICE,
                                  KW_SERVICE_V2, &service);
}

static void drain_worker(size_t index)
{
    struct buddy_request *request;
    int sent;
    struct timespec now;
    if (!host->worker_online(host->context, index) ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;
    if (now.tv_sec < next_send[index].tv_sec ||
        (now.tv_sec == next_send[index].tv_sec &&
         now.tv_nsec < next_send[index].tv_nsec))
        return;

    pthread_mutex_lock(&lock);
    request = head[index];
    if (request) {
        head[index] = request->next;
        if (!head[index])
            tail[index] = NULL;
        /* Keep this request's capacity reserved until the send succeeds. */
    }
    pthread_mutex_unlock(&lock);
    if (!request)
        return;
    next_send[index] = now;
    next_send[index].tv_nsec += 250000000L;
    if (next_send[index].tv_nsec >= 1000000000L) {
        next_send[index].tv_nsec -= 1000000000L;
        next_send[index].tv_sec++;
    }

    if (request->operation == BUDDY_ADD)
        sent = host->ao_buddy_add(host->context, request->worker_index,
                                  request->uid, request->tag,
                                  request->tag_len);
    else
        sent = host->ao_buddy_remove(host->context, request->worker_index,
                                     request->uid);
    if (!sent) {
        pthread_mutex_lock(&lock);
        request->next = head[index];
        head[index] = request;
        if (!tail[index])
            tail[index] = request;
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_mutex_lock(&lock);
    queue_size--;
    pthread_mutex_unlock(&lock);
    free(request->tag);
    free(request);
}

static void on_tick(void)
{
    for (size_t i = 0; i < host->worker_count(host->context); ++i)
        drain_worker(i);
}

static void on_event(const struct kw_ao_event *event)
{
    kw_buddy_sink_fn sink;
    void *context;
    if (event->type != KW_AO_BUDDY_STATUS &&
        event->type != KW_AO_BUDDY_REMOVED)
        return;

    pthread_mutex_lock(&lock);
    if (event->type == KW_AO_BUDDY_STATUS)
        remember_buddy(event->worker_index, event->uid);
    else
        forget_buddy(event->worker_index, event->uid);
    sink = event_sink;
    context = event_sink_context;
    pthread_mutex_unlock(&lock);
    if (sink)
        sink(context, event->worker_index, event->uid, event->online,
             event->type == KW_AO_BUDDY_REMOVED);
}

static void destroy(void)
{
    struct buddy_request *request;
    set_sink(NULL, NULL);
    pthread_mutex_lock(&lock);
    request = NULL;
    for (size_t i = 0; i < KW_MAX_WORKERS; ++i) {
        if (tail[i]) {
            tail[i]->next = request;
            request = head[i];
        }
        head[i] = tail[i] = NULL;
    }
    memset(next_send, 0, sizeof(next_send));
    queue_size = 0;
    memset(buddy_count, 0, sizeof(buddy_count));
    memset(buddy_ids, 0, sizeof(buddy_ids));
    pthread_mutex_unlock(&lock);
    while (request) {
        struct buddy_request *next = request->next;
        free(request->tag);
        free(request);
        request = next;
    }
    host = NULL;
}

static const struct kw_plugin_v2 plugin = {
    KW_PLUGIN_ABI_V2, sizeof(struct kw_plugin_v2), "buddy", 10,
    plugin_init, NULL, on_event, on_tick, NULL, destroy
};

const struct kw_plugin_v2 *kworker_plugin_v2(void)
{
    return &plugin;
}
