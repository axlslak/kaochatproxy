#include "kworker_plugin.h"
#include "kworker_services.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TELL_QUEUE_LIMIT 8192

struct tell_request {
    size_t worker_index;
    uint32_t uid;
    char *message;
    size_t message_len;
    struct tell_request *next;
};

static const struct kw_host_v2 *host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct tell_request *head, *tail;
static size_t queue_size;
static size_t queued_count[KW_MAX_WORKERS];
static kw_tell_sink_fn event_sink;
static void *event_sink_context;
static struct timespec next_send[KW_MAX_WORKERS];

static int send_due(size_t worker_index)
{
    struct timespec now;
    if (worker_index >= KW_MAX_WORKERS ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    if (now.tv_sec < next_send[worker_index].tv_sec ||
        (now.tv_sec == next_send[worker_index].tv_sec &&
         now.tv_nsec < next_send[worker_index].tv_nsec))
        return 0;
    next_send[worker_index] = now;
    next_send[worker_index].tv_sec++;
    return 1;
}

static int enqueue_tell(size_t worker_index, uint32_t uid,
                        const char *message, size_t message_len)
{
    struct tell_request *request;
    if (!host || worker_index >= host->worker_count(host->context) ||
        !uid || !message || !message_len || message_len > 65535)
        return 0;
    request = calloc(1, sizeof(*request));
    if (!request)
        return 0;
    request->message = malloc(message_len);
    if (!request->message) {
        free(request);
        return 0;
    }
    memcpy(request->message, message, message_len);
    request->worker_index = worker_index;
    request->uid = uid;
    request->message_len = message_len;

    pthread_mutex_lock(&lock);
    if (queue_size == TELL_QUEUE_LIMIT) {
        pthread_mutex_unlock(&lock);
        free(request->message);
        free(request);
        return 0;
    }
    if (tail)
        tail->next = request;
    else
        head = request;
    tail = request;
    queue_size++;
    queued_count[worker_index]++;
    pthread_mutex_unlock(&lock);
    return 1;
}

static int set_sink(kw_tell_sink_fn sink, void *context)
{
    pthread_mutex_lock(&lock);
    event_sink = sink;
    event_sink_context = context;
    pthread_mutex_unlock(&lock);
    return 1;
}

static size_t queued(size_t worker_index)
{
    size_t result = 0;
    pthread_mutex_lock(&lock);
    if (worker_index < KW_MAX_WORKERS)
        result = queued_count[worker_index];
    pthread_mutex_unlock(&lock);
    return result;
}

static struct tell_request *take_for_worker(size_t worker_index)
{
    struct tell_request *request, *previous = NULL;
    pthread_mutex_lock(&lock);
    request = head;
    while (request && request->worker_index != worker_index) {
        previous = request;
        request = request->next;
    }
    if (request) {
        if (previous)
            previous->next = request->next;
        else
            head = request->next;
        if (tail == request)
            tail = previous;
        request->next = NULL;
        queue_size--;
        queued_count[worker_index]--;
    }
    pthread_mutex_unlock(&lock);
    return request;
}

static void put_back(struct tell_request *request)
{
    pthread_mutex_lock(&lock);
    request->next = head;
    head = request;
    if (!tail)
        tail = request;
    queue_size++;
    queued_count[request->worker_index]++;
    pthread_mutex_unlock(&lock);
}

static const struct kw_tell_service_v2 service = {
    enqueue_tell, set_sink, queued
};

static int plugin_init(const struct kw_host_v2 *host_api)
{
    if (!host_api || host_api->abi_version != KW_PLUGIN_ABI_V2 ||
        host_api->struct_size < sizeof(*host_api))
        return 0;
    host = host_api;
    return host->register_service(host->context, KW_TELL_SERVICE,
                                  KW_SERVICE_V2, &service);
}

static void on_tick(void)
{
    size_t count = host->worker_count(host->context);
    for (size_t worker_index = 0; worker_index < count; ++worker_index) {
        struct tell_request *request;
        int sent;
        if (!host->worker_online(host->context, worker_index) ||
            !send_due(worker_index))
            continue;
        request = take_for_worker(worker_index);
        if (!request)
            continue;
        sent = host->ao_tell(host->context, worker_index, request->uid,
                             request->message, request->message_len);
        if (!sent) {
            put_back(request);
            continue;
        }
        free(request->message);
        free(request);
    }
}

static void on_event(const struct kw_ao_event *event)
{
    kw_tell_sink_fn sink;
    void *context;
    if (event->type != KW_AO_PRIVATE_MESSAGE)
        return;
    pthread_mutex_lock(&lock);
    sink = event_sink;
    context = event_sink_context;
    pthread_mutex_unlock(&lock);
    if (sink)
        sink(context, event->worker_index, event->uid,
             event->text, event->text_len);
}

static void destroy(void)
{
    struct tell_request *request;
    set_sink(NULL, NULL);
    pthread_mutex_lock(&lock);
    request = head;
    head = tail = NULL;
    queue_size = 0;
    memset(queued_count, 0, sizeof(queued_count));
    memset(next_send, 0, sizeof(next_send));
    pthread_mutex_unlock(&lock);
    while (request) {
        struct tell_request *next = request->next;
        free(request->message);
        free(request);
        request = next;
    }
    host = NULL;
}

static const struct kw_plugin_v2 plugin = {
    KW_PLUGIN_ABI_V2, sizeof(struct kw_plugin_v2), "tell", 20,
    plugin_init, NULL, on_event, on_tick, NULL, destroy
};

const struct kw_plugin_v2 *kworker_plugin_v2(void)
{
    return &plugin;
}

