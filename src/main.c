#include "kworker_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <aochat.h>

volatile sig_atomic_t kw_stop_requested;
volatile sig_atomic_t kw_reload_requested;

/* Start connection attempts in small batches without blocking the AO loop. */
#define KW_LOGIN_BATCH_SIZE 5u
#define KW_LOGIN_BATCH_INTERVAL_SECONDS 2

enum login_stage { WAIT_SEED, WAIT_CHARLIST, WAIT_LOGIN_OK, ONLINE };

static const char *login_stage_name(enum login_stage stage)
{
    static const char *names[] = {
        "waiting-login-seed", "waiting-character-list",
        "waiting-login-ok", "online"
    };
    return stage >= WAIT_SEED && stage <= ONLINE ? names[stage] : "unknown";
}

static void report_worker_failure(const struct kw_runtime *runtime,
                                  size_t worker_index, const char *reason,
                                  int packet_type, int error_code)
{
    const struct kw_worker_state *worker = &runtime->workers[worker_index];
    fprintf(stderr, "Kworker %s stopped: %s (worker=%zu, stage=%s",
            runtime->config.workers[worker_index].character, reason,
            worker_index + 1, login_stage_name(worker->stage));
    if (packet_type >= 0)
        fprintf(stderr, ", packet=%d", packet_type);
    if (error_code)
        fprintf(stderr, ", error=%d: %s", error_code, strerror(error_code));
    fputs(").\n", stderr);
}

static void signal_handler(int signal_number)
{
    if (signal_number == SIGHUP)
        kw_reload_requested = 1;
    else
        kw_stop_requested = 1;
}

static int install_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = signal_handler;
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL))
        return 0;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL) == 0;
}

static time_t monotonic_seconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? now.tv_sec : (time_t)-1;
}

static uint32_t decoded_u32(const void *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

static int has_arg(const aocMessage *message, int index, int type)
{
    return message && index >= 0 && index < message->argc && message->argv &&
        message->argt && message->argl && message->argv[index] &&
        message->argt[index] == type && message->argl[index] >= 0 &&
        (type != AOC_TYPE_INTEGER || message->argl[index] == 4);
}

void kw_log(void *context, enum kw_log_level level,
            const char *plugin, const char *message)
{
    static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    (void)context;
    fprintf(stderr, "[%s] [%s] %s\n", names[level], plugin, message);
}

static int register_service(void *context, const char *name,
                            uint32_t version, const void *service)
{
    struct kw_runtime *runtime = context;
    if (!name || !*name || !service || runtime->service_count == KW_MAX_SERVICES)
        return 0;
    for (size_t i = 0; i < runtime->service_count; ++i)
        if (strcmp(runtime->services[i].name, name) == 0 &&
            runtime->services[i].version == version)
            return 0;
    if (strlen(name) >= sizeof(runtime->services[0].name))
        return 0;
    strcpy(runtime->services[runtime->service_count].name, name);
    runtime->services[runtime->service_count].version = version;
    runtime->services[runtime->service_count].service = service;
    runtime->service_count++;
    return 1;
}

static const void *find_service(void *context, const char *name, uint32_t version)
{
    struct kw_runtime *runtime = context;
    for (size_t i = 0; i < runtime->service_count; ++i)
        if (runtime->services[i].version == version &&
            strcmp(runtime->services[i].name, name) == 0)
            return runtime->services[i].service;
    return NULL;
}

static int host_buddy_add(void *context, size_t worker_index, uint32_t uid,
                          const char *tag, size_t tag_len)
{
    struct kw_runtime *runtime = context;
    struct kw_worker_state *worker;
    aocConnection *connection;
    if (worker_index >= runtime->config.worker_count || tag_len > INT_MAX)
        return 0;
    worker = &runtime->workers[worker_index];
    connection = worker->ao_connection;
    if (!worker->online || !connection)
        return 0;
    return aocSendBuddyAdd(connection, uid, (const unsigned char *)tag,
                           (int)tag_len) > 0;
}

static int host_buddy_remove(void *context, size_t worker_index, uint32_t uid)
{
    struct kw_runtime *runtime = context;
    struct kw_worker_state *worker;
    aocConnection *connection;
    if (worker_index >= runtime->config.worker_count)
        return 0;
    worker = &runtime->workers[worker_index];
    connection = worker->ao_connection;
    return worker->online && connection &&
           aocSendBuddyRemove(connection, uid) > 0;
}

static int host_tell(void *context, size_t worker_index, uint32_t uid,
                     const char *message, size_t message_len)
{
    struct kw_runtime *runtime = context;
    struct kw_worker_state *worker;
    aocConnection *connection;
    static const unsigned char tag[] = { 0 };
    if (worker_index >= runtime->config.worker_count || message_len > INT_MAX)
        return 0;
    worker = &runtime->workers[worker_index];
    connection = worker->ao_connection;
    if (!worker->online || !connection)
        return 0;
    return aocSendPrivateMessage(connection, uid, message, (int)message_len,
                                 tag, 1) > 0;
}

static size_t host_worker_count(void *context)
{
    return ((struct kw_runtime *)context)->config.worker_count;
}

static const char *host_worker_name(void *context, size_t worker_index)
{
    struct kw_runtime *runtime = context;
    if (worker_index >= runtime->config.worker_count)
        return NULL;
    return runtime->config.workers[worker_index].character;
}

static uint32_t host_worker_uid(void *context, size_t worker_index)
{
    struct kw_runtime *runtime = context;
    return worker_index < runtime->config.worker_count ?
        runtime->workers[worker_index].uid : 0;
}

static int host_worker_online(void *context, size_t worker_index)
{
    struct kw_runtime *runtime = context;
    return worker_index < runtime->config.worker_count &&
           runtime->workers[worker_index].online;
}

static int select_character(struct kw_runtime *runtime, size_t worker_index,
                            aocConnection *connection,
                            const aocMessage *message)
{
    const char *wanted = runtime->config.workers[worker_index].character;
    int count = aocMsgArraySize(message, 0);
    if (count < 0 || aocMsgArraySize(message, 1) != count)
        return 0;
    for (int i = 0; i < count; ++i) {
        int nt, nl, it, il;
        const char *name = aocMsgArrayValue(message, 1, i, &nt, &nl);
        const void *id = aocMsgArrayValue(message, 0, i, &it, &il);
        if (!name || !id || nt != AOC_TYPE_STRING ||
            it != AOC_TYPE_INTEGER || il != 4)
            return 0;
        if (strcasecmp(name, wanted) != 0)
            continue;
        runtime->workers[worker_index].uid = decoded_u32(id);
        return aocSendLoginSelectChar(
            connection, runtime->workers[worker_index].uid) > 0;
    }
    return 0;
}

static int dispatch_message(struct kw_runtime *runtime, size_t worker_index,
                            aocConnection *connection,
                            const aocMessage *message)
{
    struct kw_worker_state *worker = &runtime->workers[worker_index];
    struct kw_account *account = &runtime->config.workers[worker_index];
    struct kw_ao_event event;
    memset(&event, 0, sizeof(event));
    event.worker_index = worker_index;

    switch (message->type) {
    case AOC_SRV_LOGIN_SEED: {
        char *key;
        int sent;
        if (worker->stage != WAIT_SEED ||
            !has_arg(message, 0, AOC_TYPE_STRING))
            return 0;
        key = aocKeyexGenerateKey(AOC_STR(message->argv[0]),
                                 account->username, account->password);
        if (!key)
            return 0;
        sent = aocSendLoginResponse(connection, 0, account->username, key);
        aocFree(key);
        if (sent <= 0)
            return 0;
        worker->stage = WAIT_CHARLIST;
        return 1;
    }
    case AOC_SRV_LOGIN_CHARLIST:
        if (worker->stage != WAIT_CHARLIST ||
            !select_character(runtime, worker_index, connection, message))
            return 0;
        worker->stage = WAIT_LOGIN_OK;
        return 1;
    case AOC_SRV_LOGIN_OK:
        if (worker->stage != WAIT_LOGIN_OK)
            return 0;
        worker->stage = ONLINE;
        worker->online = 1;
        event.type = KW_AO_ONLINE;
        event.uid = worker->uid;
        kw_plugins_event(runtime, &event);
        fprintf(stderr, "Kworker %zu/%zu %s logged in (uid=%" PRIu32 ").\n",
                worker_index + 1, runtime->config.worker_count,
                account->character, worker->uid);
        return 1;
    case AOC_SRV_LOGIN_ERROR:
        fprintf(stderr, "AO login rejected for worker %zu (%s).\n",
                worker_index + 1, account->character);
        return 0;
    case AOC_SRV_BUDDY_STATUS:
        if (!has_arg(message, 0, AOC_TYPE_INTEGER) ||
            !has_arg(message, 1, AOC_TYPE_INTEGER))
            return 0;
        event.type = KW_AO_BUDDY_STATUS;
        event.uid = decoded_u32(message->argv[0]);
        event.online = decoded_u32(message->argv[1]) != 0;
        kw_plugins_event(runtime, &event);
        return 1;
    case AOC_SRV_BUDDY_REMOVED:
        if (!has_arg(message, 0, AOC_TYPE_INTEGER))
            return 0;
        event.type = KW_AO_BUDDY_REMOVED;
        event.uid = decoded_u32(message->argv[0]);
        kw_plugins_event(runtime, &event);
        return 1;
    case AOC_SRV_PRIVATE_MSG:
        if (!has_arg(message, 0, AOC_TYPE_INTEGER) ||
            !has_arg(message, 1, AOC_TYPE_STRING))
            return 0;
        event.type = KW_AO_PRIVATE_MESSAGE;
        event.uid = decoded_u32(message->argv[0]);
        event.text = AOC_STR(message->argv[1]);
        event.text_len = (size_t)message->argl[1];
        kw_plugins_event(runtime, &event);
        return 1;
    case AOC_SRV_CLIENT_NAME:
    case AOC_SRV_LOOKUP_RESULT:
        if (!has_arg(message, 0, AOC_TYPE_INTEGER) ||
            !has_arg(message, 1, AOC_TYPE_STRING))
            return 0;
        event.type = message->type == AOC_SRV_CLIENT_NAME ?
            KW_AO_CLIENT_NAME : KW_AO_LOOKUP_RESULT;
        event.uid = decoded_u32(message->argv[0]);
        event.text = AOC_STR(message->argv[1]);
        event.text_len = (size_t)message->argl[1];
        kw_plugins_event(runtime, &event);
        return 1;
    default:
        return 1;
    }
}

static size_t connection_index(struct kw_runtime *runtime,
                               const aocConnection *connection)
{
    for (size_t i = 0; i < runtime->config.worker_count; ++i)
        if (runtime->workers[i].ao_connection == connection)
            return i;
    return runtime->config.worker_count;
}

static void close_workers(struct kw_runtime *runtime)
{
    for (size_t i = 0; i < runtime->config.worker_count; ++i) {
        aocConnection *connection = runtime->workers[i].ao_connection;
        if (connection) {
            aocDisconnect(connection);
            aocFree(connection);
        }
        runtime->workers[i].ao_connection = NULL;
        runtime->workers[i].online = 0;
    }
}

static int run_workers(struct kw_runtime *runtime)
{
    aocConnection *connections[KW_MAX_WORKERS] = { 0 };
    struct sockaddr_in *address;
    time_t now;
    size_t started_workers = 0;
    struct timespec next_batch = { 0, 0 };
    size_t failed_worker = 0;
    const char *failure_reason = "unknown worker failure";
    int failure_packet = -1, failure_error = 0;
    static const unsigned char ping[] = "kworker";

    address = aocMakeAddr(runtime->config.chat_host, runtime->config.chat_port);
    if (!address) {
        fputs("Cannot resolve AO chat server.\n", stderr);
        return 0;
    }
    aocSetPref(AOC_PREF_FASTRND, (void *)(intptr_t)1);
    while (!kw_stop_requested) {
        aocEvent *event;
        struct timespec batch_now;
        if (clock_gettime(CLOCK_MONOTONIC, &batch_now) != 0) {
            failure_reason = "monotonic clock failed";
            failure_error = errno;
            goto failed;
        }
        if (started_workers < runtime->config.worker_count &&
            (batch_now.tv_sec > next_batch.tv_sec ||
             (batch_now.tv_sec == next_batch.tv_sec &&
              batch_now.tv_nsec >= next_batch.tv_nsec))) {
            size_t batch_first = started_workers;
            size_t batch_end = started_workers + KW_LOGIN_BATCH_SIZE;
            if (batch_end > runtime->config.worker_count)
                batch_end = runtime->config.worker_count;
            for (; started_workers < batch_end; ++started_workers) {
                size_t i = started_workers;
                struct kw_worker_state *worker = &runtime->workers[i];
                now = monotonic_seconds();
                if (now == (time_t)-1) {
                    failed_worker = i;
                    failure_reason = "monotonic clock failed";
                    failure_error = errno;
                    goto failed;
                }
                worker->stage = WAIT_SEED;
                worker->login_deadline = (long)now + KW_LOGIN_TIMEOUT;
                worker->next_ping = (long)now + KW_PING_INTERVAL;
                connections[i] = aocInit(NULL);
                worker->ao_connection = connections[i];
                if (!connections[i] || !aocConnect(connections[i], address)) {
                    failed_worker = i;
                    failure_reason = "could not start AO connection";
                    failure_error = errno;
                    goto failed;
                }
            }
            fprintf(stderr, "Starting Kworker connections %zu-%zu/%zu.\n",
                    batch_first + 1, started_workers, runtime->config.worker_count);
            if (clock_gettime(CLOCK_MONOTONIC, &next_batch) != 0) {
                failure_reason = "monotonic clock failed";
                failure_error = errno;
                goto failed;
            }
            next_batch.tv_sec += KW_LOGIN_BATCH_INTERVAL_SECONDS;
        }
        if (!aocPollArray(0, 250000, (int)started_workers,
                          connections)) {
            if (errno == EINTR)
                continue;
            failure_reason = "AO socket poll failed";
            failure_error = errno;
            goto failed;
        }
        while ((event = aocEventGet()) != NULL) {
            size_t worker_index = connection_index(runtime, event->aoc);
            int keep = 1;
            if (worker_index == runtime->config.worker_count) {
                aocEventDestroy(event);
                continue;
            }
            if (event->type == AOC_EVENT_MESSAGE) {
                const aocMessage *message = event->data;
                keep = dispatch_message(runtime, worker_index,
                                        connections[worker_index], message);
                if (!keep) {
                    failure_reason = "rejected or unexpected AO packet";
                    failure_packet = message ? message->type : -1;
                }
            } else if (event->type == AOC_EVENT_CONNFAIL) {
                failure_reason = "AO connection failed";
                failure_error = (int)(intptr_t)event->data;
                keep = 0;
            } else if (event->type == AOC_EVENT_DISCONNECT) {
                failure_reason = "AO server disconnected Kworker";
                failure_error = (int)(intptr_t)event->data;
                keep = 0;
            }
            aocEventDestroy(event);
            if (!keep) {
                failed_worker = worker_index;
                goto failed;
            }
        }

        if (kw_reload_requested) {
            kw_reload_requested = 0;
            if (!kw_plugins_reload(runtime)) {
                failure_reason = "plugin reload failed";
                goto failed;
            }
            for (size_t i = 0; i < runtime->config.worker_count; ++i) {
                if (runtime->workers[i].online) {
                    struct kw_ao_event online = {
                        KW_AO_ONLINE, i, runtime->workers[i].uid, 0, NULL, 0
                    };
                    kw_plugins_event(runtime, &online);
                }
            }
        }

        kw_plugins_tick(runtime);
        now = monotonic_seconds();
        if (now == (time_t)-1) {
            failure_reason = "monotonic clock failed";
            failure_error = errno;
            goto failed;
        }
        for (size_t i = 0; i < started_workers; ++i) {
            struct kw_worker_state *worker = &runtime->workers[i];
            if (worker->stage != ONLINE && (long)now >= worker->login_deadline) {
                failed_worker = i;
                failure_reason = "AO login timed out";
                goto failed;
            }
            if (worker->stage == ONLINE && (long)now >= worker->next_ping) {
                if (aocSendPing(connections[i], ping,
                                (int)sizeof(ping) - 1) <= 0) {
                    failed_worker = i;
                    failure_reason = "AO keepalive send failed";
                    failure_error = errno;
                    goto failed;
                }
                worker->next_ping = (long)now + KW_PING_INTERVAL;
            }
        }
    }

    close_workers(runtime);
    return 1;

failed:
    report_worker_failure(runtime, failed_worker, failure_reason,
                          failure_packet, failure_error);
    close_workers(runtime);
    return 0;
}

int main(void)
{
    struct kw_runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    if (!kw_config_load(&runtime.config) || !install_signals())
        return EXIT_FAILURE;

    runtime.host_api = (struct kw_host_v2) {
        .abi_version = KW_PLUGIN_ABI_V2,
        .struct_size = sizeof(struct kw_host_v2),
        .context = &runtime,
        .log = kw_log,
        .register_service = register_service,
        .find_service = find_service,
        .ao_buddy_add = host_buddy_add,
        .ao_buddy_remove = host_buddy_remove,
        .ao_tell = host_tell,
        .worker_count = host_worker_count,
        .worker_name = host_worker_name,
        .worker_uid = host_worker_uid,
        .worker_online = host_worker_online
    };

    if (!kw_plugins_load(&runtime))
        return EXIT_FAILURE;
    int ok = run_workers(&runtime);
    kw_plugins_stop(&runtime);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

