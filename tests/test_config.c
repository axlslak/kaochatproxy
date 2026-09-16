#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/kworker_internal.h"

int main(void)
{
    static const char contents[] =
        "# two independent AO accounts\n"
        "[worker]\n"
        "username=account-one\n"
        "password=secret-one\n"
        "character=Workerone\n"
        "\n"
        "[worker]\n"
        "username=account-two\n"
        "password=secret-two\n"
        "character=Workertwo\n";
    char path[] = "/tmp/kworker-config-XXXXXX";
    struct kw_config config;
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, contents, sizeof(contents) - 1) ==
           (ssize_t)(sizeof(contents) - 1));
    assert(close(fd) == 0);
    assert(setenv("KWORKER_CONFIG", path, 1) == 0);
    assert(setenv("AOPORT", "7106", 1) == 0);
    assert(kw_config_load(&config));
    assert(config.worker_count == 2);
    assert(strcmp(config.workers[0].character, "Workerone") == 0);
    assert(strcmp(config.workers[1].username, "account-two") == 0);
    assert(config.chat_port == 7106);
    assert(unlink(path) == 0);
    return 0;
}
