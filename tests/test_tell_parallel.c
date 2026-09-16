#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../plugins/tell.c"

static unsigned sent[3];

static size_t mock_worker_count(void *context)
{
    (void)context;
    return 3;
}

static int mock_worker_online(void *context, size_t worker_index)
{
    (void)context;
    return worker_index < 3;
}

static int mock_tell(void *context, size_t worker_index, uint32_t uid,
                     const char *message, size_t message_len)
{
    (void)context;
    assert(worker_index < 3 && uid && message && message_len);
    sent[worker_index]++;
    return 1;
}

int main(void)
{
    static const struct kw_host_v2 mock_host = {
        .abi_version = KW_PLUGIN_ABI_V2,
        .struct_size = sizeof(struct kw_host_v2),
        .ao_tell = mock_tell,
        .worker_count = mock_worker_count,
        .worker_online = mock_worker_online
    };
    static const char message[] = "hello";

    host = &mock_host;
    assert(enqueue_tell(0, 1001, message, sizeof(message) - 1));
    assert(enqueue_tell(1, 1002, message, sizeof(message) - 1));
    assert(enqueue_tell(2, 1003, message, sizeof(message) - 1));
    on_tick();
    assert(sent[0] == 1 && sent[1] == 1 && sent[2] == 1);
    assert(queue_size == 0);
    destroy();
    return 0;
}
