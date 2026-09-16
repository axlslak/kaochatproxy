#include <assert.h>
#include "../plugins/buddy.c"

static unsigned sent[KW_MAX_WORKERS];
static int blocked;
static size_t workers(void *context) { (void)context; return 15; }
static int online(void *context, size_t i) { (void)context; return i != 0; }
static int send_add(void *context, size_t i, uint32_t uid,
                    const char *tag, size_t length)
{
    (void)context; (void)uid; (void)tag; (void)length;
    if (blocked && i == 1)
        return 0;
    sent[i]++;
    return 1;
}
static int send_remove(void *context, size_t i, uint32_t uid)
{
    return send_add(context, i, uid, NULL, 0);
}
int main(void)
{
    const struct kw_host_v2 api = {
        .worker_count = workers, .worker_online = online,
        .ao_buddy_add = send_add, .ao_buddy_remove = send_remove
    };
    host = &api;
    for (uint32_t uid = 1; uid <= 10000; ++uid)
        assert(add_buddy(uid % 15, uid, NULL, 0) == 1);
    assert(queue_size == 10000);
    assert(add_buddy(1, 1, NULL, 0) == 1);
    assert(queue_size == 10000); /* identical pending add coalesces */
    blocked = 1;
    on_tick();
    assert(sent[0] == 0 && sent[1] == 0 && sent[14] == 1);
    assert(queue_size == 10000 - 13);
    on_tick(); /* pacing: no immediate second burst */
    assert(sent[14] == 1);
    blocked = 0;
    memset(next_send, 0, sizeof(next_send));
    on_tick();
    assert(sent[1] == 1 && sent[14] == 2);
    assert(remove_buddy(0, 15) == 1);
    assert(add_buddy(0, 15, NULL, 0) == 1); /* preserve opposite ops */
    size_t saved = queue_size;
    queue_size = BUDDY_QUEUE_LIMIT;
    assert(add_buddy(0, 20000, NULL, 0) == -1);
    queue_size = saved;
    destroy();
    assert(queue_size == 0);
    for (size_t i = 0; i < KW_MAX_WORKERS; ++i)
        assert(!head[i] && !tail[i]);
    return 0;
}
