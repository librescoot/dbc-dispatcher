#define main dispatcher_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void check_commands(const char *path, const char *expected) {
    char actual[512] = {0};
    FILE *f = fopen(path, "r");
    assert(f);
    assert(fread(actual, 1, sizeof(actual) - 1, f) > 0);
    fclose(f);
    assert(!strcmp(actual, expected));
    assert(unlink(path) == 0);
}

int main(void) {
    char directory[] = "/tmp/dbc-logserver-test-XXXXXX";
    assert(mkdtemp(directory));
    char script[PATH_MAX], commands[PATH_MAX];
    snprintf(script, sizeof(script), "%s/systemctl", directory);
    snprintf(commands, sizeof(commands), "%s/commands", directory);
    FILE *f = fopen(script, "w");
    assert(f);
    assert(fputs("#!/bin/sh\n"
                 "printf '%s\\n' \"$*\" >>\"$DISPATCHER_TEST_COMMANDS\"\n"
                 "case \"$1\" in\n"
                 "  is-enabled) [ \"$DISPATCHER_TEST_ENABLED\" = 1 ] ;;\n"
                 "  stop|disable) exit \"$DISPATCHER_TEST_ACTION_STATUS\" ;;\n"
                 "  *) exit 99 ;;\n"
                 "esac\n", f) >= 0);
    assert(fclose(f) == 0);
    assert(chmod(script, 0700) == 0);
    const char *old_path = getenv("PATH");
    char *path = NULL;
    assert(asprintf(&path, "%s:%s", directory, old_path ? old_path : "") >= 0);
    assert(setenv("PATH", path, 1) == 0);
    free(path);
    assert(setenv("DISPATCHER_TEST_COMMANDS", commands, 1) == 0);
    assert(setenv("DISPATCHER_TEST_ACTION_STATUS", "0", 1) == 0);

    const char *stop = "is-enabled --quiet systemd-journal-upload.service\n"
                       "stop systemd-journal-upload.service\n";
    const char *disable = "is-enabled --quiet systemd-journal-upload.service\n"
                          "disable --now systemd-journal-upload.service\n";
    assert(setenv("DISPATCHER_TEST_ENABLED", "0", 1) == 0);
    assert(stop_log_upload() == 0);
    check_commands(commands, stop);
    assert(stop_log_upload() == 0);
    check_commands(commands, stop);

    assert(setenv("DISPATCHER_TEST_ENABLED", "1", 1) == 0);
    assert(stop_log_upload() == 0);
    check_commands(commands, disable);

    assert(setenv("DISPATCHER_TEST_ACTION_STATUS", "7", 1) == 0);
    assert(stop_log_upload() == 7);
    check_commands(commands, disable);
    assert(setenv("DISPATCHER_TEST_ENABLED", "0", 1) == 0);
    assert(stop_log_upload() == 7);
    check_commands(commands, stop);

    assert(unlink(script) == 0);
    assert(rmdir(directory) == 0);
    return 0;
}
