#include "kworker_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char *trim(char *text)
{
    char *end;
    while (*text && isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = 0;
    if (end - text >= 2 && text[0] == '"' && end[-1] == '"') {
        text++;
        end[-1] = 0;
    }
    return text;
}

static int copy_text(char *destination, size_t size,
                     const char *value, const char *label)
{
    size_t length;
    if (!value)
        return 0;
    length = strlen(value);
    if (!length || length >= size) {
        fprintf(stderr, "%s must contain 1..%zu bytes.\n", label, size - 1);
        return 0;
    }
    memcpy(destination, value, length + 1);
    return 1;
}

static int parse_port(const char *value, int fallback)
{
    char *end = NULL;
    long port;
    if (!value || !*value)
        return fallback;
    errno = 0;
    port = strtol(value, &end, 10);
    if (errno || !end || *end || port < 1 || port > 65535)
        return -1;
    return (int)port;
}

static int validate_workers(struct kw_config *config)
{
    if (!config->worker_count) {
        fputs("No Kworker accounts configured.\n", stderr);
        return 0;
    }
    for (size_t i = 0; i < config->worker_count; ++i) {
        struct kw_account *account = &config->workers[i];
        if (!account->username[0] || !account->password[0] ||
            !account->character[0]) {
            fprintf(stderr, "Worker section %zu requires username, password and character.\n",
                    i + 1);
            return 0;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcasecmp(account->character,
                           config->workers[j].character) == 0) {
                fprintf(stderr, "Duplicate worker character %s.\n",
                        account->character);
                return 0;
            }
        }
    }
    return 1;
}

static int load_file(struct kw_config *config, const char *path, int required)
{
    FILE *file;
    char line[4096];
    size_t line_number = 0;
    struct kw_account *current = NULL;
    struct stat status;

    file = fopen(path, "r");
    if (!file) {
        if (!required && errno == ENOENT)
            return 1;
        fprintf(stderr, "Cannot open Kworker config %s: %s.\n",
                path, strerror(errno));
        return 0;
    }
    if (stat(path, &status) == 0 && (status.st_mode & 0077))
        fprintf(stderr, "Warning: %s contains AO passwords and is readable beyond its owner.\n",
                path);

    while (fgets(line, sizeof(line), file)) {
        char *key, *value, *equals;
        line_number++;
        key = trim(line);
        if (!*key || *key == '#' || *key == ';')
            continue;
        if (strcmp(key, "[worker]") == 0) {
            if (config->worker_count == KW_MAX_WORKERS) {
                fprintf(stderr, "%s:%zu: maximum %u workers exceeded.\n",
                        path, line_number, KW_MAX_WORKERS);
                fclose(file);
                return 0;
            }
            current = &config->workers[config->worker_count++];
            memset(current, 0, sizeof(*current));
            continue;
        }
        equals = strchr(key, '=');
        if (!equals) {
            fprintf(stderr, "%s:%zu: expected key=value or [worker].\n",
                    path, line_number);
            fclose(file);
            return 0;
        }
        *equals = 0;
        value = trim(equals + 1);
        key = trim(key);
        if (!current) {
            fprintf(stderr, "%s:%zu: worker field outside [worker].\n",
                    path, line_number);
            fclose(file);
            return 0;
        }
        if (strcmp(key, "username") == 0) {
            if (!copy_text(current->username, sizeof(current->username),
                           value, "worker username"))
                goto invalid;
        } else if (strcmp(key, "password") == 0) {
            if (!copy_text(current->password, sizeof(current->password),
                           value, "worker password"))
                goto invalid;
        } else if (strcmp(key, "character") == 0) {
            if (!copy_text(current->character, sizeof(current->character),
                           value, "worker character"))
                goto invalid;
        } else {
            fprintf(stderr, "%s:%zu: unknown worker key %s.\n",
                    path, line_number, key);
            goto invalid;
        }
    }
    if (ferror(file)) {
        fprintf(stderr, "Cannot read Kworker config %s.\n", path);
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;

invalid:
    fclose(file);
    return 0;
}

static int load_legacy_environment(struct kw_config *config)
{
    const char *username = getenv("AOUSER");
    const char *password = getenv("AOPASS");
    const char *character = getenv("AOCHAR");
    if (!username || !*username || !password || !*password ||
        !character || !*character) {
        fputs("Create kworker.conf or set nonempty AOUSER, AOPASS and AOCHAR.\n",
              stderr);
        return 0;
    }
    config->worker_count = 1;
    return copy_text(config->workers[0].username,
                     sizeof(config->workers[0].username),
                     username, "AOUSER") &&
           copy_text(config->workers[0].password,
                     sizeof(config->workers[0].password),
                     password, "AOPASS") &&
           copy_text(config->workers[0].character,
                     sizeof(config->workers[0].character),
                     character, "AOCHAR");
}

int kw_config_load(struct kw_config *config)
{
    const char *config_path = getenv("KWORKER_CONFIG");
    const char *value;
    int required = config_path && *config_path;

    memset(config, 0, sizeof(*config));
    strcpy(config->plugin_dir, "./kworker-plugins");
    strcpy(config->chat_host, "chat.d1.funcom.com");
    config->chat_port = 7105;

    if (!required)
        config_path = "kworker.conf";
    if (!load_file(config, config_path, required))
        return 0;
    if (!config->worker_count && !load_legacy_environment(config))
        return 0;

    value = getenv("KWORKER_PLUGIN_DIR");
    if (value && *value &&
        !copy_text(config->plugin_dir, sizeof(config->plugin_dir),
                   value, "KWORKER_PLUGIN_DIR"))
        return 0;
    value = getenv("AOSERVER");
    if (value && *value &&
        !copy_text(config->chat_host, sizeof(config->chat_host),
                   value, "AOSERVER"))
        return 0;
    config->chat_port = parse_port(getenv("AOPORT"), config->chat_port);
    if (config->chat_port < 0) {
        fputs("AOPORT must be in the range 1..65535.\n", stderr);
        return 0;
    }
    return validate_workers(config);
}

