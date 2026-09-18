#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "kworker_plugin.h"
#include "kworker_services.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define AO_LOGIN_OK 5u
#define AO_CLIENT_NAME 20u
#define AO_LOOKUP_RESULT 21u
#define AO_MSG_PRIVATE 30u
#define AO_BUDDY_ADD 40u
#define AO_BUDDY_REMOVE 41u
#define AO_PING 100u
#define BUDDY_ROUTE_LIMIT (1000u * (KW_MAX_WORKERS + 1u))

/* Proxy-local connection membership: slot zero is the client upstream;
 * worker indices remain independent of it. */
#define MEMBERSHIP_WORDS ((KW_MAX_WORKERS + 1u + 63u) / 64u)
struct connection_memberships {
    uint64_t words[MEMBERSHIP_WORDS];
};

static int membership_has(const struct connection_memberships *set, int destination)
{
    size_t slot = (size_t)(destination + 1);
    return !!(set->words[slot / 64u] & (UINT64_C(1) << (slot % 64u)));
}

static void membership_set(struct connection_memberships *set, int destination)
{
    size_t slot = (size_t)(destination + 1);
    set->words[slot / 64u] |= UINT64_C(1) << (slot % 64u);
}

static void membership_clear(struct connection_memberships *set, int destination)
{
    size_t slot = (size_t)(destination + 1);
    set->words[slot / 64u] &= ~(UINT64_C(1) << (slot % 64u));
}

static int membership_any(const struct connection_memberships *set)
{
    for (size_t i = 0; i < MEMBERSHIP_WORDS; ++i)
        if (set->words[i])
            return 1;
    return 0;
}

struct buddy_route {
    uint32_t uid;
    int destination;
    struct connection_memberships memberships;
};

struct frame_node {
    unsigned char *data;
    size_t length;
    struct frame_node *next;
};

static const struct kw_host_v2 *host;
static const struct kw_buddy_service_v2 *buddy;
static const struct kw_tell_service_v2 *tell;
static pthread_t proxy_thread;
static int thread_started;
static atomic_int stop_requested;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int listener_fd = -1, client_fd = -1, upstream_fd = -1;
static int worker_online[KW_MAX_WORKERS], worker_announced[KW_MAX_WORKERS];
static int main_online;
static struct frame_node *frame_head, *frame_tail;
static struct buddy_route *buddy_routes;
static size_t buddy_route_capacity;
static size_t buddy_route_count, main_buddy_count;
static size_t worker_buddy_count[KW_MAX_WORKERS];
static size_t tell_round_robin;
static time_t started_at;
static atomic_int route_failed;
static void observe_buddy(uint32_t uid, int destination, int removed);

static uint16_t get_u16(const unsigned char *value)
{
    return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}

static uint32_t get_u32(const unsigned char *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | value[3];
}

static void put_u16(unsigned char *value, uint16_t number)
{
    value[0] = (unsigned char)(number >> 8);
    value[1] = (unsigned char)number;
}

static void put_u32(unsigned char *value, uint32_t number)
{
    value[0] = (unsigned char)(number >> 24);
    value[1] = (unsigned char)(number >> 16);
    value[2] = (unsigned char)(number >> 8);
    value[3] = (unsigned char)number;
}

static int parse_string(const unsigned char *body, size_t body_len, size_t *position,
                        const unsigned char **text, size_t *text_len)
{
    uint16_t length;
    if (*position > body_len || body_len - *position < 2)
        return 0;
    length = get_u16(body + *position);
    *position += 2;
    if (body_len - *position < length)
        return 0;
    *text = body + *position;
    *text_len = length;
    *position += length;
    return 1;
}

static unsigned char *make_frame(uint16_t type, const unsigned char *body,
                                 size_t body_len, size_t *frame_len)
{
    unsigned char *frame;
    if (body_len > UINT16_MAX)
        return NULL;
    frame = malloc(body_len + 4);
    if (!frame)
        return NULL;
    put_u16(frame, type);
    put_u16(frame + 2, (uint16_t)body_len);
    if (body_len)
        memcpy(frame + 4, body, body_len);
    *frame_len = body_len + 4;
    return frame;
}

static int queue_frame(unsigned char *frame, size_t length)
{
    struct frame_node *node = malloc(sizeof(*node));
    if (!node) {
        free(frame);
        return 0;
    }
    node->data = frame;
    node->length = length;
    node->next = NULL;
    pthread_mutex_lock(&state_lock);
    if (frame_tail)
        frame_tail->next = node;
    else
        frame_head = node;
    frame_tail = node;
    pthread_mutex_unlock(&state_lock);
    return 1;
}

static int queue_empty_frame(uint16_t type)
{
    size_t length;
    unsigned char *frame = make_frame(type, NULL, 0, &length);
    return frame && queue_frame(frame, length);
}

static int send_all(int fd, const unsigned char *data, size_t length)
{
    while (length) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent > 0) {
            data += (size_t)sent;
            length -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        return 0;
    }
    return 1;
}

static int recv_all(int fd, unsigned char *data, size_t length)
{
    while (length) {
        ssize_t received = recv(fd, data, length, 0);
        if (received > 0) {
            data += (size_t)received;
            length -= (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        return 0;
    }
    return 1;
}

static unsigned char *recv_frame(int fd, size_t *frame_len)
{
    unsigned char header[4];
    unsigned char *frame;
    uint16_t body_len;
    if (!recv_all(fd, header, sizeof(header)))
        return NULL;
    body_len = get_u16(header + 2);
    frame = malloc((size_t)body_len + 4);
    if (!frame)
        return NULL;
    memcpy(frame, header, 4);
    if (body_len && !recv_all(fd, frame + 4, body_len)) {
        free(frame);
        return NULL;
    }
    *frame_len = (size_t)body_len + 4;
    return frame;
}

static int connect_tcp(const char *name, const char *port)
{
    struct addrinfo hints, *addresses = NULL, *address;
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(name, port, &hints, &addresses) != 0)
        return -1;
    for (address = addresses; address; address = address->ai_next) {
        int flags, connected = 0;
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
            if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
                connected = 1;
            } else if (errno == EINPROGRESS) {
                for (int waited = 0; waited < 40 && !atomic_load(&stop_requested); ++waited) {
                    struct pollfd pfd = { fd, POLLOUT, 0 };
                    int ready = poll(&pfd, 1, 250);
                    if (ready > 0) {
                        int socket_error = 0;
                        socklen_t error_len = sizeof(socket_error);
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                       &socket_error, &error_len) == 0 && !socket_error)
                            connected = 1;
                        break;
                    }
                    if (ready < 0 && errno != EINTR)
                        break;
                }
            }
        }
        if (connected) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int select_private_ipv4(char *buffer, size_t size)
{
    struct ifaddrs *interfaces = NULL, *interface;
    int found = 0;
    if (!buffer || size < INET_ADDRSTRLEN || getifaddrs(&interfaces) != 0)
        return 0;
    for (interface = interfaces; interface; interface = interface->ifa_next) {
        const struct sockaddr_in *address;
        uint32_t host_address;
        if (!interface->ifa_addr ||
            interface->ifa_addr->sa_family != AF_INET ||
            !(interface->ifa_flags & IFF_UP) ||
            (interface->ifa_flags & IFF_LOOPBACK))
            continue;
        address = (const struct sockaddr_in *)interface->ifa_addr;
        host_address = ntohl(address->sin_addr.s_addr);
        if ((host_address & 0xffff0000u) != 0xc0a80000u)
            continue;
        if (inet_ntop(AF_INET, &address->sin_addr, buffer, size)) {
            found = 1;
            break;
        }
    }
    freeifaddrs(interfaces);
    return found;
}

static int create_listener(const char *name, const char *port)
{
    struct addrinfo hints, *addresses = NULL, *address;
    int fd = -1, yes = 1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(name, port, &hints, &addresses) != 0)
        return -1;
    for (address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(fd, 1) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static void worker_tag(size_t worker_index, char *buffer, size_t size)
{
    snprintf(buffer, size, "{\"id\":%zu,\"name\":\"%s\"}",
             worker_index + 1,
             host->worker_name(host->context, worker_index));
}

static void buddy_sink(void *context, size_t worker_index,
                       uint32_t uid, int online, int removed)
{
    unsigned char body[512];
    char tag[256];
    size_t tag_len, body_len, frame_len;
    unsigned char *frame;
    (void)context;
    observe_buddy(uid, (int)worker_index, removed);
    if (removed) {
        put_u32(body, uid);
        frame = make_frame(AO_BUDDY_REMOVE, body, 4, &frame_len);
    } else {
        worker_tag(worker_index, tag, sizeof(tag));
        tag_len = strlen(tag);
        put_u32(body, uid);
        put_u32(body + 4, online ? 1u : 0u);
        put_u16(body + 8, (uint16_t)tag_len);
        memcpy(body + 10, tag, tag_len);
        body_len = tag_len + 10;
        frame = make_frame(AO_BUDDY_ADD, body, body_len, &frame_len);
    }
    if (frame)
        queue_frame(frame, frame_len);
}

static void tell_sink(void *context, size_t worker_index, uint32_t uid,
                      const char *message, size_t message_len)
{
    unsigned char *body, *frame;
    char tag[256];
    size_t tag_len, body_len, frame_len, position = 0;
    (void)context;
    worker_tag(worker_index, tag, sizeof(tag));
    tag_len = strlen(tag);
    if (message_len > UINT16_MAX || tag_len > UINT16_MAX)
        return;
    body_len = 4 + 2 + message_len + 2 + tag_len;
    body = malloc(body_len);
    if (!body)
        return;
    put_u32(body + position, uid); position += 4;
    put_u16(body + position, (uint16_t)message_len); position += 2;
    memcpy(body + position, message, message_len); position += message_len;
    put_u16(body + position, (uint16_t)tag_len); position += 2;
    memcpy(body + position, tag, tag_len);
    frame = make_frame(AO_MSG_PRIVATE, body, body_len, &frame_len);
    free(body);
    if (frame)
        queue_frame(frame, frame_len);
}

static int is_capabilities(const unsigned char *text, size_t length)
{
    static const char needle[] = "capabilities";
    if (length < sizeof(needle) - 1)
        return 0;
    for (size_t i = 0; i + sizeof(needle) - 1 <= length; ++i)
        if (memcmp(text + i, needle, sizeof(needle) - 1) == 0)
            return 1;
    return 0;
}

static int answer_capabilities(void)
{
    /* Configured character names are at most 15 bytes; allow ample JSON overhead. */
    char json[512u + 32u * KW_MAX_WORKERS];
    unsigned char body[sizeof(json) + 2u];
    size_t json_len = 0, frame_len;
    unsigned char *frame;
    int length = snprintf(json, sizeof(json),
        "{\"name\":\"kworker-nady-proxy\",\"version\":\"0.1.0\","
        "\"type\":\"capabilities\",\"supported-cmds\":[\"capabilities\"],"
        "\"rate-limited\":true,\"default-mode\":\"round-robin\","
        "\"workers\":[");
    if (length < 0 || (size_t)length >= sizeof(json))
        return 0;
    json_len = (size_t)length;
    for (size_t i = 0; i < host->worker_count(host->context); ++i) {
        length = snprintf(json + json_len, sizeof(json) - json_len,
                          "%s\"%s\"", i ? "," : "",
                          host->worker_name(host->context, i));
        if (length < 0 || (size_t)length >= sizeof(json) - json_len)
            return 0;
        json_len += (size_t)length;
    }
    length = snprintf(json + json_len, sizeof(json) - json_len,
        "],\"started-at\":%lld,"
        "\"send-modes\":[\"round-robin\",\"by-charid\",\"by-msgid\","
        "\"proxy-default\",\"by-worker\"],"
        "\"buddy-modes\":[\"by-worker\"]}",
        (long long)started_at);
    if (length < 0 || (size_t)length >= sizeof(json) - json_len)
        return 0;
    json_len += (size_t)length;
    put_u16(body, (uint16_t)json_len);
    memcpy(body + 2, json, json_len);
    frame = make_frame(AO_PING, body, json_len + 2, &frame_len);
    return frame && queue_frame(frame, frame_len);
}

static int contains_text(const unsigned char *text, size_t text_len,
                         const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > text_len)
        return 0;
    for (size_t i = 0; i + needle_len <= text_len; ++i)
        if (memcmp(text + i, needle, needle_len) == 0)
            return 1;
    return 0;
}

static int json_number(const unsigned char *text, size_t text_len,
                       const char *key, size_t *value)
{
    size_t key_len = strlen(key);
    for (size_t i = 0; i + key_len <= text_len; ++i) {
        size_t position;
        size_t number = 0;
        int found = 0;
        if (memcmp(text + i, key, key_len) != 0)
            continue;
        position = i + key_len;
        while (position < text_len &&
               (text[position] == ' ' || text[position] == '\t'))
            position++;
        if (position >= text_len || text[position++] != ':')
            continue;
        while (position < text_len &&
               (text[position] == ' ' || text[position] == '\t'))
            position++;
        while (position < text_len && text[position] >= '0' &&
               text[position] <= '9') {
            if (number > (SIZE_MAX - 9u) / 10u)
                return 0;
            number = number * 10u + (size_t)(text[position++] - '0');
            found = 1;
        }
        if (found) {
            *value = number;
            return 1;
        }
    }
    return 0;
}

static int forced_destination(const unsigned char *tag, size_t tag_len,
                              int *destination)
{
    size_t worker;
    if (!contains_text(tag, tag_len, "by-worker"))
        return 0;
    if (!json_number(tag, tag_len, "\"worker\"", &worker))
        return -1;
    if (worker == 0) {
        *destination = -1;
        return 1;
    }
    if (worker > host->worker_count(host->context))
        return -1;
    *destination = (int)(worker - 1);
    return 1;
}

static int least_loaded_destination(void)
{
    int destination = main_buddy_count < 1000 ? -1 : -2;
    size_t least = main_buddy_count < 1000 ? main_buddy_count : 999;
    size_t count = host->worker_count(host->context);
    for (size_t i = 0; i < count; ++i) {
        if (!worker_online[i])
            continue;
        if (worker_buddy_count[i] <= least) {
            destination = (int)i;
            least = worker_buddy_count[i];
        }
    }
    return destination;
}

static void log_buddy_allocation(size_t total)
{
    char message[64u + 48u * KW_MAX_WORKERS];
    size_t position;
    pthread_mutex_lock(&state_lock);
    int length = snprintf(message, sizeof(message),
                          "buddy allocation: leader=%zu", main_buddy_count);
    if (length < 0 || (size_t)length >= sizeof(message))
        goto done;
    position = (size_t)length;
    for (size_t i = 0; i < host->worker_count(host->context); ++i) {
        length = snprintf(message + position, sizeof(message) - position,
                          ", worker%zu=%zu", i + 1, worker_buddy_count[i]);
        if (length < 0 || (size_t)length >= sizeof(message) - position)
            goto done;
        position += (size_t)length;
    }
    if (total % 100u == 0u)
        host->log(host->context, KW_LOG_INFO, "nady_proxy", message);
done:
    pthread_mutex_unlock(&state_lock);
}

/* Called with state_lock held. Bound storage by the maximum supported AO
 * capacity, not a small bootstrap constant. Grow only when needed. */
static int reserve_route(void)
{
    if (buddy_route_count < buddy_route_capacity)
        return 1;
    if (buddy_route_capacity == BUDDY_ROUTE_LIMIT)
        return 0;
    size_t capacity = buddy_route_capacity ? buddy_route_capacity * 2 : 256;
    if (capacity > BUDDY_ROUTE_LIMIT)
        capacity = BUDDY_ROUTE_LIMIT;
    struct buddy_route *routes = realloc(buddy_routes, capacity * sizeof(*routes));
    if (!routes)
        return 0;
    buddy_routes = routes;
    buddy_route_capacity = capacity;
    return 1;
}

static void change_count(int destination, int add)
{
    size_t *count = destination < 0 ? &main_buddy_count :
        &worker_buddy_count[destination];
    if (add)
        ++*count;
    else if (*count)
        --*count;
}

/* Status packets also describe buddies not added during this proxy session.
 * Keep duplicate memberships: they occupy a slot on EACH connection. */
static void observe_buddy(uint32_t uid, int destination, int removed)
{
    if (destination < -1 || destination >= (int)host->worker_count(host->context))
        return;
    pthread_mutex_lock(&state_lock);
    size_t i;
    for (i = 0; i < buddy_route_count; ++i)
        if (buddy_routes[i].uid == uid)
            break;
    if (i == buddy_route_count) {
        if (removed) {
            pthread_mutex_unlock(&state_lock);
            return;
        }
        if (!reserve_route()) {
            atomic_store(&route_failed, 1);
            pthread_mutex_unlock(&state_lock);
            return;
        }
        buddy_routes[i] = (struct buddy_route){ .uid = uid, .destination = destination };
        buddy_route_count++;
    }
    struct buddy_route *route = &buddy_routes[i];
    if (!removed && !membership_has(&route->memberships, destination)) {
        membership_set(&route->memberships, destination);
        change_count(destination, 1);
    } else if (removed && membership_has(&route->memberships, destination)) {
        membership_clear(&route->memberships, destination);
        change_count(destination, 0);
        if (!membership_any(&route->memberships)) {
            buddy_routes[i] = buddy_routes[--buddy_route_count];
        } else if (route->destination == destination) {
            for (int d = -1; d < (int)host->worker_count(host->context); ++d)
                if (membership_has(&route->memberships, d)) {
                    route->destination = d;
                    break;
                }
        }
    }
    pthread_mutex_unlock(&state_lock);
}

static int assign_buddy_route(uint32_t uid, int requested_destination,
                              int has_forced_destination, int *destination,
                              int *is_new)
{
    size_t total;
    pthread_mutex_lock(&state_lock);
    for (size_t i = 0; i < buddy_route_count; ++i) {
        if (buddy_routes[i].uid != uid)
            continue;
        *destination = buddy_routes[i].destination;
        *is_new = 0;
        pthread_mutex_unlock(&state_lock);
        return 1;
    }
    if (!reserve_route()) {
        pthread_mutex_unlock(&state_lock);
        return 0;
    }
    *destination = has_forced_destination ?
        requested_destination : least_loaded_destination();
    if (*destination < -1 || *destination >= (int)host->worker_count(host->context) ||
        (*destination < 0 ? main_buddy_count : worker_buddy_count[*destination]) >= 1000) {
        pthread_mutex_unlock(&state_lock);
        host->log(host->context, KW_LOG_ERROR, "nady_proxy",
                  "buddy capacity exhausted on requested/available connections");
        return 0;
    }
    buddy_routes[buddy_route_count].uid = uid;
    buddy_routes[buddy_route_count].destination = *destination;
    memset(&buddy_routes[buddy_route_count].memberships, 0,
           sizeof(buddy_routes[buddy_route_count].memberships));
    membership_set(&buddy_routes[buddy_route_count].memberships, *destination);
    buddy_route_count++;
    if (*destination >= 0)
        worker_buddy_count[*destination]++;
    else
        main_buddy_count++;
    total = buddy_route_count;
    *is_new = 1;
    pthread_mutex_unlock(&state_lock);
    log_buddy_allocation(total);
    return 1;
}

static int take_buddy_route(uint32_t uid, int *destination)
{
    pthread_mutex_lock(&state_lock);
    for (size_t i = 0; i < buddy_route_count; ++i) {
        if (buddy_routes[i].uid != uid)
            continue;
        *destination = buddy_routes[i].destination;
        for (int d = -1; d < (int)host->worker_count(host->context); ++d)
            if (membership_has(&buddy_routes[i].memberships, d))
                change_count(d, 0);
        buddy_routes[i] = buddy_routes[buddy_route_count - 1];
        buddy_route_count--;
        pthread_mutex_unlock(&state_lock);
        return 1;
    }
    pthread_mutex_unlock(&state_lock);
    return 0;
}

static int send_main_buddy_add(int upstream, uint32_t uid)
{
    unsigned char body[7] = { 0 };
    unsigned char *frame;
    size_t frame_len;
    int sent;
    put_u32(body, uid);
    put_u16(body + 4, 1);
    frame = make_frame(AO_BUDDY_ADD, body, sizeof(body), &frame_len);
    if (!frame)
        return 0;
    sent = send_all(upstream, frame, frame_len);
    free(frame);
    return sent;
}

static int online_worker_from(size_t start)
{
    size_t count = host->worker_count(host->context);
    int destination = -1;
    if (!count)
        return -1;
    pthread_mutex_lock(&state_lock);
    for (size_t offset = 0; offset < count; ++offset) {
        size_t index = (start + offset) % count;
        if (worker_online[index]) {
            destination = (int)index;
            break;
        }
    }
    pthread_mutex_unlock(&state_lock);
    return destination;
}

static int select_tell_destination(uint32_t uid,
                                   const unsigned char *tag, size_t tag_len)
{
    int destination;
    size_t number, count = host->worker_count(host->context);
    int forced = forced_destination(tag, tag_len, &destination);
    if (forced > 0)
        return destination;
    if (forced < 0)
        return -2;
    if (contains_text(tag, tag_len, "by-charid"))
        return online_worker_from(count ? uid % count : 0);
    if (contains_text(tag, tag_len, "by-msgid")) {
        if (!json_number(tag, tag_len, "\"msgid\"", &number))
            return -2;
        return online_worker_from(count ? number % count : 0);
    }
    if (contains_text(tag, tag_len, "round-robin") ||
        contains_text(tag, tag_len, "proxy-default") ||
        contains_text(tag, tag_len, "spam")) {
        destination = online_worker_from(tell_round_robin++);
        return destination;
    }
    return -1;
}

static int send_main_tell(int upstream, uint32_t uid,
                          const unsigned char *message, size_t message_len)
{
    unsigned char *body, *frame;
    size_t body_len, frame_len, position = 0;
    int sent;
    if (message_len > UINT16_MAX)
        return 0;
    body_len = 4 + 2 + message_len + 2 + 1;
    body = calloc(1, body_len);
    if (!body)
        return 0;
    put_u32(body + position, uid); position += 4;
    put_u16(body + position, (uint16_t)message_len); position += 2;
    memcpy(body + position, message, message_len); position += message_len;
    put_u16(body + position, 1);
    frame = make_frame(AO_MSG_PRIVATE, body, body_len, &frame_len);
    free(body);
    if (!frame)
        return 0;
    sent = send_all(upstream, frame, frame_len);
    free(frame);
    return sent;
}

/* Only the proxy producer waits; the AO owner keeps draining buddy queues.
 * Shutdown remains interruptible even when a worker is not online. */
static int submit_buddy(size_t index, uint32_t uid, int remove)
{
    static const char tag[] = { 0 };
    while (!atomic_load(&stop_requested)) {
        int result = remove ? buddy->remove(index, uid) :
            buddy->add(index, uid, tag, sizeof(tag));
        if (result >= 0)
            return result;
        struct timespec delay = { 0, 20000000L };
        nanosleep(&delay, NULL);
    }
    return 0;
}

static int handle_client_frame(const unsigned char *frame, size_t frame_len, int upstream)
{
    uint16_t type = get_u16(frame), body_len = get_u16(frame + 2);
    const unsigned char *body = frame + 4, *first, *second;
    size_t position, first_len, second_len;
    uint32_t uid;
    if ((size_t)body_len + 4 != frame_len)
        return 0;
    if (type == AO_BUDDY_ADD) {
        int destination = -1, is_new, forced;
        if (body_len < 6)
            return 0;
        uid = get_u32(body);
        position = 4;
        if (!parse_string(body, body_len, &position, &first, &first_len) ||
            position != body_len)
            return 0;
        forced = forced_destination(first, first_len, &destination);
        if (forced < 0)
            return 1;
        if (!assign_buddy_route(uid, destination, forced > 0,
                                &destination, &is_new))
            return 0;
        (void)is_new;
        if (destination >= 0)
            return submit_buddy((size_t)destination, uid, 0);
        return send_main_buddy_add(upstream, uid);
    }
    if (type == AO_BUDDY_REMOVE) {
        int destination;
        if (body_len != 4)
            return 0;
        uid = get_u32(body);
        struct connection_memberships memberships = { { 0 } };
        pthread_mutex_lock(&state_lock);
        for (size_t i = 0; i < buddy_route_count; ++i)
            if (buddy_routes[i].uid == uid) {
                memberships = buddy_routes[i].memberships;
                break;
            }
        pthread_mutex_unlock(&state_lock);
        if (!take_buddy_route(uid, &destination)) {
            int worker_ok = 1;
            for (size_t i = 0; i < host->worker_count(host->context); ++i)
                if (!submit_buddy(i, uid, 1))
                    worker_ok = 0;
            int main_ok = send_all(upstream, frame, frame_len);
            return worker_ok && main_ok;
        }
        int ok = 1;
        for (size_t i = 0; i < host->worker_count(host->context); ++i)
            if (membership_has(&memberships, (int)i) && !submit_buddy(i, uid, 1))
                ok = 0;
        if (membership_has(&memberships, -1) && !send_all(upstream, frame, frame_len))
            ok = 0;
        return ok;
    }
    if (type == AO_MSG_PRIVATE) {
        if (body_len < 8)
            return 0;
        uid = get_u32(body);
        position = 4;
        if (!parse_string(body, body_len, &position, &first, &first_len) ||
            !parse_string(body, body_len, &position, &second, &second_len) ||
            position != body_len)
            return 0;
        if (second_len && second[0] != 0) {
            int destination = select_tell_destination(uid, second, second_len);
            if (destination == -2)
                return 1;
            if (destination >= 0)
                return tell->enqueue((size_t)destination, uid,
                                     (const char *)first, first_len);
            return send_main_tell(upstream, uid, first, first_len);
        }
    }
    if (type == AO_PING) {
        position = 0;
        if (parse_string(body, body_len, &position, &first, &first_len) &&
            position == body_len && is_capabilities(first, first_len))
            return answer_capabilities();
    }
    return send_all(upstream, frame, frame_len);
}

static int drain_frames(int client)
{
    for (;;) {
        struct frame_node *node;
        pthread_mutex_lock(&state_lock);
        /*
         * Worker packets may accumulate before Nadybot connects.  Do not put
         * any of them on the leader stream until Funcom has completed that
         * stream's login handshake.
         */
        if (!main_online) {
            pthread_mutex_unlock(&state_lock);
            return 1;
        }
        node = frame_head;
        if (node) {
            frame_head = node->next;
            if (!frame_head)
                frame_tail = NULL;
        }
        pthread_mutex_unlock(&state_lock);
        if (!node)
            return 1;
        int ok = send_all(client, node->data, node->length);
        free(node->data);
        free(node);
        if (!ok)
            return 0;
    }
}

static int proxy_session(int client, int upstream)
{
    struct pollfd fds[2] = { { client, POLLIN, 0 }, { upstream, POLLIN, 0 } };
    pthread_mutex_lock(&state_lock);
    main_online = 0;
    memset(worker_announced, 0, sizeof(worker_announced));
    pthread_mutex_unlock(&state_lock);
    while (!atomic_load(&stop_requested)) {
        if (atomic_load(&route_failed)) {
            host->log(host->context, KW_LOG_ERROR, "nady_proxy",
                      "cannot track observed buddies: route allocation failed");
            return 0;
        }
        if (!drain_frames(client)) {
            host->log(host->context, KW_LOG_WARN, "nady_proxy",
                      "write to Nadybot client failed");
            return 0;
        }
        int ready = poll(fds, 2, 250);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            host->log(host->context, KW_LOG_WARN, "nady_proxy",
                      "proxy session poll failed");
            return 0;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            host->log(host->context, KW_LOG_WARN, "nady_proxy",
                      "Nadybot client socket closed");
            return 0;
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            host->log(host->context, KW_LOG_WARN, "nady_proxy",
                      "Funcom leader socket closed");
            return 0;
        }
        if (fds[0].revents & POLLIN) {
            size_t length;
            unsigned char *frame = recv_frame(client, &length);
            if (!frame) {
                host->log(host->context, KW_LOG_WARN, "nady_proxy",
                          "could not read Nadybot client packet");
                return 0;
            }
            int ok = handle_client_frame(frame, length, upstream);
            free(frame);
            if (!ok) {
                host->log(host->context, KW_LOG_WARN, "nady_proxy",
                          "Nadybot packet was invalid or could not be routed");
                return 0;
            }
        }
        if (fds[1].revents & POLLIN) {
            size_t length;
            unsigned char *frame = recv_frame(upstream, &length);
            if (!frame) {
                host->log(host->context, KW_LOG_WARN, "nady_proxy",
                          "could not read Funcom leader packet");
                return 0;
            }
            uint16_t type = get_u16(frame);
            if (type == AO_BUDDY_ADD && length >= 14)
                observe_buddy(get_u32(frame + 4), -1, 0);
            else if (type == AO_BUDDY_REMOVE && length == 8)
                observe_buddy(get_u32(frame + 4), -1, 1);
            if (!send_all(client, frame, length)) {
                host->log(host->context, KW_LOG_WARN, "nady_proxy",
                          "could not relay Funcom packet to Nadybot");
                free(frame);
                return 0;
            }
            free(frame);
            if (type == AO_LOGIN_OK) {
                size_t announcements = 0;
                pthread_mutex_lock(&state_lock);
                main_online = 1;
                for (size_t i = 0; i < host->worker_count(host->context); ++i) {
                    if (worker_online[i] && !worker_announced[i]) {
                        worker_announced[i] = 1;
                        announcements++;
                    }
                }
                pthread_mutex_unlock(&state_lock);
                /*
                 * Announce the already-online Kworker before draining its
                 * queued packets.  The leader LoginOk was sent just above.
                 */
                while (announcements--) {
                    unsigned char login_ok[4];
                    put_u16(login_ok, AO_LOGIN_OK);
                    put_u16(login_ok + 2, 0);
                    if (!send_all(client, login_ok, sizeof(login_ok))) {
                        host->log(host->context, KW_LOG_WARN, "nady_proxy",
                                  "could not announce Kworker to Nadybot");
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static void publish_socket(int *slot, int value)
{
    pthread_mutex_lock(&state_lock);
    *slot = value;
    pthread_mutex_unlock(&state_lock);
}

static void forget_leader_buddies(void)
{
    pthread_mutex_lock(&state_lock);
    for (size_t i = 0; i < buddy_route_count;) {
        struct buddy_route *route = &buddy_routes[i];
        membership_clear(&route->memberships, -1);
        if (!membership_any(&route->memberships)) {
            buddy_routes[i] = buddy_routes[--buddy_route_count];
            continue;
        }
        if (route->destination == -1)
            for (size_t d = 0; d < host->worker_count(host->context); ++d)
                if (membership_has(&route->memberships, (int)d)) {
                    route->destination = (int)d;
                    break;
                }
        i++;
    }
    main_buddy_count = 0;
    main_online = 0;
    pthread_mutex_unlock(&state_lock);
}

static void *thread_main(void *unused)
{
    const char *listen_host = getenv("NADY_LISTEN_HOST");
    const char *listen_source = "NADY_LISTEN_HOST";
    const char *listen_port = getenv("NADY_PORT");
    const char *upstream_host = getenv("NADY_UPSTREAM_HOST");
    const char *upstream_port = getenv("NADY_UPSTREAM_PORT");
    char detected_host[INET_ADDRSTRLEN];
    char log_message[256];
    int listener;
    (void)unused;
    if (!listen_host || !*listen_host) {
        if (select_private_ipv4(detected_host, sizeof(detected_host))) {
            listen_host = detected_host;
            listen_source = "detected private LAN";
        } else {
            listen_host = "127.0.0.1";
            listen_source = "safe loopback fallback";
        }
    }
    if (!listen_port || !*listen_port) listen_port = "9993";
    if (!upstream_host || !*upstream_host) upstream_host = "chat.d1.funcom.com";
    if (!upstream_port || !*upstream_port) upstream_port = "7105";
    listener = create_listener(listen_host, listen_port);
    if (listener < 0) {
        host->log(host->context, KW_LOG_ERROR, "nady_proxy", "cannot open listener");
        return NULL;
    }
    publish_socket(&listener_fd, listener);
    snprintf(log_message, sizeof(log_message),
             "listener ready on %s:%s (%s)",
             listen_host, listen_port, listen_source);
    host->log(host->context, KW_LOG_INFO, "nady_proxy", log_message);
    while (!atomic_load(&stop_requested)) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        publish_socket(&client_fd, client);
        int upstream = connect_tcp(upstream_host, upstream_port);
        if (upstream >= 0) {
            publish_socket(&upstream_fd, upstream);
            proxy_session(client, upstream);
            forget_leader_buddies();
            close(upstream);
            publish_socket(&upstream_fd, -1);
        }
        close(client);
        publish_socket(&client_fd, -1);
    }
    close(listener);
    publish_socket(&listener_fd, -1);
    return NULL;
}

static int plugin_init(const struct kw_host_v2 *host_api)
{
    if (!host_api || host_api->abi_version != KW_PLUGIN_ABI_V2 ||
        host_api->struct_size < sizeof(*host_api))
        return 0;
    host = host_api;
    buddy = host->find_service(host->context, KW_BUDDY_SERVICE, KW_SERVICE_V2);
    tell = host->find_service(host->context, KW_TELL_SERVICE, KW_SERVICE_V2);
    if (!buddy || !tell)
        return 0;
    started_at = time(NULL);
    if (!buddy->set_sink(buddy_sink, NULL))
        return 0;
    if (!tell->set_sink(tell_sink, NULL)) {
        buddy->set_sink(NULL, NULL);
        return 0;
    }
    return 1;
}

static int plugin_start(void)
{
    atomic_store(&stop_requested, 0);
    if (pthread_create(&proxy_thread, NULL, thread_main, NULL) != 0)
        return 0;
    thread_started = 1;
    return 1;
}

static void on_event(const struct kw_ao_event *event)
{
    int announce = 0;
    if (event->type == KW_AO_CLIENT_NAME || event->type == KW_AO_LOOKUP_RESULT) {
        unsigned char *body, *frame;
        size_t body_len, frame_len;
        uint16_t type = event->type == KW_AO_CLIENT_NAME ?
            AO_CLIENT_NAME : AO_LOOKUP_RESULT;
        if (event->text_len > UINT16_MAX)
            return;
        body_len = 4 + 2 + event->text_len;
        body = malloc(body_len);
        if (!body)
            return;
        put_u32(body, event->uid);
        put_u16(body + 4, (uint16_t)event->text_len);
        memcpy(body + 6, event->text, event->text_len);
        frame = make_frame(type, body, body_len, &frame_len);
        free(body);
        if (frame)
            queue_frame(frame, frame_len);
        return;
    }
    if (event->type != KW_AO_ONLINE)
        return;
    if (event->worker_index >= host->worker_count(host->context))
        return;
    pthread_mutex_lock(&state_lock);
    worker_online[event->worker_index] = 1;
    if (main_online && !worker_announced[event->worker_index]) {
        worker_announced[event->worker_index] = 1;
        announce = 1;
    }
    pthread_mutex_unlock(&state_lock);
    if (announce)
        queue_empty_frame(AO_LOGIN_OK);
}

static void plugin_stop(void)
{
    atomic_store(&stop_requested, 1);
    pthread_mutex_lock(&state_lock);
    if (listener_fd >= 0) shutdown(listener_fd, SHUT_RDWR);
    if (client_fd >= 0) shutdown(client_fd, SHUT_RDWR);
    if (upstream_fd >= 0) shutdown(upstream_fd, SHUT_RDWR);
    pthread_mutex_unlock(&state_lock);
    if (thread_started)
        pthread_join(proxy_thread, NULL);
    thread_started = 0;
}

static void plugin_destroy(void)
{
    struct frame_node *node;
    buddy->set_sink(NULL, NULL);
    tell->set_sink(NULL, NULL);
    pthread_mutex_lock(&state_lock);
    node = frame_head;
    frame_head = frame_tail = NULL;
    buddy_route_count = main_buddy_count = tell_round_robin = 0;
    free(buddy_routes);
    buddy_routes = NULL;
    buddy_route_capacity = 0;
    atomic_store(&route_failed, 0);
    memset(worker_buddy_count, 0, sizeof(worker_buddy_count));
    memset(worker_online, 0, sizeof(worker_online));
    memset(worker_announced, 0, sizeof(worker_announced));
    main_online = 0;
    pthread_mutex_unlock(&state_lock);
    while (node) {
        struct frame_node *next = node->next;
        free(node->data);
        free(node);
        node = next;
    }
    buddy = NULL;
    tell = NULL;
    host = NULL;
}

static const struct kw_plugin_v2 plugin = {
    KW_PLUGIN_ABI_V2, sizeof(struct kw_plugin_v2), "nady_proxy", 30,
    plugin_init, plugin_start, on_event, NULL, plugin_stop, plugin_destroy
};

const struct kw_plugin_v2 *kworker_plugin_v2(void)
{
    return &plugin;
}
