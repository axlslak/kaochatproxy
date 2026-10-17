#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <sys/time.h>

#include "../plugins/nady_proxy.c"

static size_t test_workers = 3;
static unsigned submit_calls;
static int allocation_last_worker_logged;
static struct connection_memberships removed_on;
static unsigned lookup_calls;

static int mock_lookup(void *context, size_t index, const char *name)
{
    (void)context;
    assert(index < test_workers && worker_online[index]);
    assert(strcmp(name, "Kavey") == 0);
    lookup_calls++;
    return 1;
}

static int mock_add(size_t index, uint32_t uid, const char *tag, size_t len)
{
    (void)index; (void)uid; (void)tag; (void)len;
    return ++submit_calls == 1 ? -1 : 1;
}

static int mock_remove(size_t index, uint32_t uid)
{
    (void)uid;
    membership_set(&removed_on, (int)index);
    return 1;
}

static void mock_log(void *context, enum kw_log_level level,
                     const char *plugin_name, const char *message)
{
    (void)context; (void)level; (void)plugin_name;
    if (strstr(message, "worker256="))
        allocation_last_worker_logged = 1;
}

static size_t mock_worker_count(void *context)
{
    (void)context;
    return test_workers;
}

static const char *mock_worker_name(void *context, size_t worker_index)
{
    static const char *names[] = { "Workerone", "Workertwo", "Workerthree" };
    (void)context;
    static char large_name[16];
    if (worker_index < 3)
        return names[worker_index];
    snprintf(large_name, sizeof(large_name), "Worker%09u", (unsigned)(worker_index % 1000));
    return large_name;
}

static void test_worker_service(void)
{
    int sockets[2], destination, is_new;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct timeval timeout = { 1, 0 };
    assert(setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    worker_only = 1;
    main_online = 0;
    client_fd = sockets[0];
    test_workers = 3;

    /* Capabilities are available without any AO leader or LoginOK. */
    assert(answer_capabilities() && drain_frames(sockets[0]));
    size_t length;
    unsigned char *frame = recv_frame(sockets[1], &length);
    assert(frame && get_u16(frame) == AO_PING);
    assert(contains_text(frame + 6, length - 6, "\"connection-mode\":\"workers\""));
    free(frame);

    /* No request can reserve a slot on the nonexistent leader. */
    assert(!assign_buddy_route(50000, -1, 1, &destination, &is_new));
    for (uint32_t uid = 50001; uid < 50007; ++uid) {
        assert(assign_buddy_route(uid, -1, 0, &destination, &is_new));
        assert(is_new && destination >= 0 && destination < 3);
    }
    assert(main_buddy_count == 0);
    for (size_t i = 0; i < test_workers; ++i)
        assert(worker_buddy_count[i] == 2);
    while (buddy_route_count)
        assert(take_buddy_route(buddy_routes[0].uid, &destination));

    /* Socket ingestion queues a lookup; only the AO-owner tick sends it. */
    const unsigned char name[] = { 0, 5, 'K', 'a', 'v', 'e', 'y' };
    frame = make_frame(AO_LOOKUP_RESULT, name, sizeof(name), &length);
    assert(frame && handle_client_frame(frame, length, -1));
    free(frame);
    assert(lookup_calls == 0 && lookup_count == 1);
    on_tick();
    assert(lookup_calls == 1 && lookup_count == 0);
    const struct kw_ao_event result = { KW_AO_LOOKUP_RESULT, 0, 1234, 0, "Kavey", 5 };
    on_event(&result);
    assert(drain_frames(sockets[0]));
    frame = recv_frame(sockets[1], &length);
    assert(frame && get_u16(frame) == AO_LOOKUP_RESULT && get_u32(frame + 4) == 1234);
    assert(length == 15 && memcmp(frame + 10, "Kavey", 5) == 0);
    free(frame);

    /* Heartbeats are answered locally, never forwarded to a leader. */
    frame = make_frame(AO_PING, name, sizeof(name), &length);
    assert(frame && handle_client_frame(frame, length, -1) && drain_frames(sockets[0]));
    free(frame);
    frame = recv_frame(sockets[1], &length);
    assert(frame && get_u16(frame) == AO_PING && length == 11);
    assert(memcmp(frame + 4, name, sizeof(name)) == 0);
    free(frame);

    /* Reconnect cleanup discards unsent work; disconnected events don't grow RAM. */
    assert(queue_lookup(name + 2, 5));
    clear_client_queues();
    assert(!lookup_head && !lookup_tail && lookup_count == 0);
    client_fd = -1;
    on_event(&result);
    assert(!frame_head && !frame_tail);
    worker_only = 0;
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    static const struct kw_host_v2 mock_host = {
        .abi_version = KW_PLUGIN_ABI_V2,
        .struct_size = sizeof(struct kw_host_v2),
        .worker_count = mock_worker_count,
        .log = mock_log,
        .worker_name = mock_worker_name,
        .ao_name_lookup = mock_lookup
    };
    int destination, is_new;

    host = &mock_host;
    worker_online[0] = worker_online[1] = worker_online[2] = 1;

    assert(forced_destination(
        (const unsigned char *)"{\"mode\":\"by-worker\",\"worker\":2}",
        strlen("{\"mode\":\"by-worker\",\"worker\":2}"), &destination) == 1);
    assert(destination == 1);
    assert(forced_destination(
        (const unsigned char *)"{\"mode\":\"by-worker\",\"worker\":0}",
        strlen("{\"mode\":\"by-worker\",\"worker\":0}"), &destination) == 1);
    assert(destination == -1);
    assert(forced_destination(
        (const unsigned char *)"{\"mode\":\"by-worker\",\"worker\":9}",
        strlen("{\"mode\":\"by-worker\",\"worker\":9}"), &destination) == -1);

    for (uint32_t uid = 1001; uid <= 1004; ++uid)
        assert(assign_buddy_route(uid, -1, 0, &destination, &is_new) && is_new);
    assert(main_buddy_count == 1);
    assert(worker_buddy_count[0] == 1);
    assert(worker_buddy_count[1] == 1);
    assert(worker_buddy_count[2] == 1);

    assert(assign_buddy_route(2001, 1, 1, &destination, &is_new));
    assert(is_new && destination == 1 && worker_buddy_count[1] == 2);
    assert(assign_buddy_route(2001, -1, 1, &destination, &is_new));
    assert(!is_new && destination == 1);
    assert(take_buddy_route(2001, &destination));
    assert(destination == 1 && worker_buddy_count[1] == 1);

    tell_round_robin = 0;
    assert(select_tell_destination(42,
        (const unsigned char *)"{\"mode\":\"round-robin\"}",
        strlen("{\"mode\":\"round-robin\"}")) == 0);
    assert(select_tell_destination(42,
        (const unsigned char *)"{\"mode\":\"round-robin\"}",
        strlen("{\"mode\":\"round-robin\"}")) == 1);
    assert(select_tell_destination(42,
        (const unsigned char *)"{\"mode\":\"by-worker\",\"worker\":3}",
        strlen("{\"mode\":\"by-worker\",\"worker\":3}")) == 2);

    /* Empty the small test before the 15-worker large roster. */
    while (buddy_route_count)
        assert(take_buddy_route(buddy_routes[0].uid, &destination));
    test_workers = 15;
    for (size_t i = 0; i < test_workers; ++i)
        worker_online[i] = 1;
    for (uint32_t uid = 1; uid <= 16000; ++uid) {
        assert(assign_buddy_route(uid, -1, 0, &destination, &is_new));
        assert(is_new);
        /* Confirmation must not double count reserved slots. */
        observe_buddy(uid, destination, 0);
    }
    assert(buddy_route_count == 16000 && buddy_route_capacity >= 16000);
    assert(main_buddy_count == 1000);
    for (size_t i = 0; i < test_workers; ++i)
        assert(worker_buddy_count[i] == 1000);
    assert(!assign_buddy_route(16001, -1, 0, &destination, &is_new));
    while (buddy_route_count)
        assert(take_buddy_route(buddy_routes[0].uid, &destination));

    /* Unsolicited status, duplicate membership, removal, reconnect. */
    observe_buddy(20001, 2, 0);
    observe_buddy(20001, 2, 0);
    observe_buddy(20001, 3, 0);
    observe_buddy(20001, -1, 0);
    assert(buddy_route_count == 1 && worker_buddy_count[2] == 1);
    assert(worker_buddy_count[3] == 1 && main_buddy_count == 1);
    assert(assign_buddy_route(20001, -1, 0, &destination, &is_new));
    assert(!is_new && destination == 2);
    observe_buddy(20001, 2, 1);
    assert(worker_buddy_count[2] == 0 && buddy_route_count == 1);
    forget_leader_buddies();
    assert(main_buddy_count == 0 && buddy_route_count == 1);
    assert(take_buddy_route(20001, &destination) && destination == 3);
    assert(worker_buddy_count[3] == 0);
    const struct kw_buddy_service_v2 mock_buddy = {
        .add = mock_add, .remove = mock_remove
    };
    buddy = &mock_buddy;
    assert(submit_buddy(1, 123, 0) && submit_calls == 2);
    atomic_store(&stop_requested, 1);
    assert(!submit_buddy(1, 123, 0));
    atomic_store(&stop_requested, 0);
    observe_buddy(30001, 2, 0);
    observe_buddy(30001, 3, 0);
    unsigned char removal[8];
    put_u16(removal, AO_BUDDY_REMOVE);
    put_u16(removal + 2, 4);
    put_u32(removal + 4, 30001);
    assert(handle_client_frame(removal, sizeof(removal), -1));
    assert(membership_has(&removed_on, 2) && membership_has(&removed_on, 3));
    for (int d = -1; d < (int)test_workers; ++d)
        assert(membership_has(&removed_on, d) == (d == 2 || d == 3));
    assert(buddy_route_count == 0);
    /* All workers, including every 64-bit boundary and the last worker. */
    test_workers = KW_MAX_WORKERS;
    for (size_t i = 0; i < test_workers; ++i)
        worker_online[i] = 1;
    assert(test_workers == 256);
    assert(BUDDY_ROUTE_LIMIT == 257000);
    observe_buddy(40001, -1, 0);
    for (int d = 0; d < (int)test_workers; ++d) {
        observe_buddy(40001, d, 0);
        observe_buddy(40001, d, 0);
        assert(worker_buddy_count[d] == 1);
    }
    assert(main_buddy_count == 1 && buddy_route_count == 1);
    forget_leader_buddies();
    assert(main_buddy_count == 0 && buddy_route_count == 1);
    assert(!membership_has(&buddy_routes[0].memberships, -1));
    memset(&removed_on, 0, sizeof(removed_on));
    put_u32(removal + 4, 40001);
    assert(handle_client_frame(removal, sizeof(removal), -1));
    assert(!membership_has(&removed_on, -1));
    for (int d = 0; d < (int)test_workers; ++d) {
        assert(membership_has(&removed_on, d));
        assert(worker_buddy_count[d] == 0);
    }
    assert(buddy_route_count == 0);

    /* Reused route storage must not retain old high-word memberships. */
    assert(assign_buddy_route(40002, 255, 1, &destination, &is_new));
    assert(is_new && destination == 255 && worker_buddy_count[255] == 1);
    for (int d = -1; d < 256; ++d)
        assert(membership_has(&buddy_routes[0].memberships, d) == (d == 255));
    observe_buddy(40002, 63, 0);
    observe_buddy(40002, 255, 1);
    assert(buddy_routes[0].destination == 63 && worker_buddy_count[255] == 0);
    observe_buddy(40002, 63, 1);
    assert(buddy_route_count == 0 && worker_buddy_count[63] == 0);
    const char last_worker[] = "{\"mode\":\"by-worker\",\"worker\":256}";
    assert(select_tell_destination(42, (const unsigned char *)last_worker,
                                   strlen(last_worker)) == 255);
    assert(!assign_buddy_route(40003, 256, 1, &destination, &is_new));

    log_buddy_allocation(100);
    assert(allocation_last_worker_logged);

    /* A complete capability frame must include the final worker. */
    assert(answer_capabilities());
    assert(frame_head && frame_head == frame_tail);
    assert(get_u16(frame_head->data) == AO_PING);
    assert((size_t)get_u16(frame_head->data + 2) + 4 == frame_head->length);
    assert((size_t)get_u16(frame_head->data + 4) + 6 == frame_head->length);
    assert(contains_text(frame_head->data + 6, frame_head->length - 6,
                         "Worker000000255"));
    assert(contains_text(frame_head->data + 6, frame_head->length - 6,
                         "\"buddy-modes\":[\"by-worker\"]}"));
    free(frame_head->data);
    free(frame_head);
    frame_head = frame_tail = NULL;
    test_worker_service();
    free(buddy_routes);
    return 0;
}
