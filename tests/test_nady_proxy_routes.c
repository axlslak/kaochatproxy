#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "../plugins/nady_proxy.c"

static size_t test_workers = 3;
static unsigned submit_calls;
static uint64_t removed_on;

static int mock_add(size_t index, uint32_t uid, const char *tag, size_t len)
{
    (void)index; (void)uid; (void)tag; (void)len;
    return ++submit_calls == 1 ? -1 : 1;
}

static int mock_remove(size_t index, uint32_t uid)
{
    (void)uid;
    removed_on |= destination_bit((int)index);
    return 1;
}

static void mock_log(void *context, enum kw_log_level level,
                     const char *plugin_name, const char *message)
{
    (void)context; (void)level; (void)plugin_name; (void)message;
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
    return worker_index < 3 ? names[worker_index] : NULL;
}

int main(void)
{
    static const struct kw_host_v2 mock_host = {
        .abi_version = KW_PLUGIN_ABI_V2,
        .struct_size = sizeof(struct kw_host_v2),
        .worker_count = mock_worker_count,
        .log = mock_log,
        .worker_name = mock_worker_name
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
    assert(removed_on == (destination_bit(2) | destination_bit(3)));
    assert(buddy_route_count == 0);
    free(buddy_routes);
    return 0;
}
