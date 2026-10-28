#define main dispatcher_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void script(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(content, f) >= 0 && fclose(f) == 0);
    assert(chmod(path, 0700) == 0);
}

static void field(redisContext *ctx, const char *key, const char *name, const char *expected) {
    redisReply *r = redisCommand(ctx, "HGET %s %s", key, name);
    assert(r && r->type == REDIS_REPLY_STRING && strcmp(r->str, expected) == 0);
    freeReplyObject(r);
}

int main(void) {
    char dir[] = "/tmp/dbc-boot-test-XXXXXX";
    assert(mkdtemp(dir));
    char printenv[PATH_MAX], setenv_path[PATH_MAX], envfile[PATH_MAX], sock[PATH_MAX];
    snprintf(printenv, sizeof(printenv), "%s/fw_printenv", dir);
    snprintf(setenv_path, sizeof(setenv_path), "%s/fw_setenv", dir);
    snprintf(envfile, sizeof(envfile), "%s/env", dir);
    snprintf(sock, sizeof(sock), "%s/redis.sock", dir);
    script(printenv, "#!/bin/sh\n[ -f \"$TEST_ENV\" ] || exit 1\nexec /bin/cat \"$TEST_ENV\"\n");
    script(setenv_path, "#!/bin/sh\nprintf '%s=%s\\n' \"$1\" \"$2\" >>\"$TEST_ENV\"\n");
    const char *old_path = getenv("PATH");
    char *path = NULL;
    assert(asprintf(&path, "%s:%s", dir, old_path ? old_path : "") >= 0);
    assert(setenv("PATH", path, 1) == 0);
    assert(setenv("TEST_ENV", envfile, 1) == 0);
    free(path);

    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        execlp("redis-server", "redis-server", "--port", "0", "--unixsocket", sock,
               "--save", "", "--appendonly", "no", "--loglevel", "warning", (char *)NULL);
        _exit(127);
    }
    redisContext *ctx = NULL;
    for (int i = 0; i < 100; i++) {
        ctx = redisConnectUnix(sock);
        if (ctx && !ctx->err) break;
        if (ctx) redisFree(ctx);
        ctx = NULL;
        usleep(10000);
    }
    assert(ctx);
    redisReply *r = redisCommand(ctx, "HSET boot:dbc theme old sound 1 note ok updated 1");
    assert(r); freeReplyObject(r);
    r = redisCommand(ctx, "HSET boot:desired sound 0");
    assert(r); freeReplyObject(r);

    /* An unavailable environment must neither publish a default nor lose a request. */
    assert(!apply_boot_desired(ctx));
    field(ctx, "boot:dbc", "theme", "old");
    field(ctx, "boot:desired", "sound", "0");

    FILE *f = fopen(envfile, "w");
    assert(f && fputs("boot_animation=librescoot-xp\n", f) >= 0 && fclose(f) == 0);
    assert(apply_boot_desired(ctx));
    field(ctx, "boot:dbc", "theme", "librescoot-xp");
    field(ctx, "boot:dbc", "sound", "0");
    field(ctx, "boot:dbc", "note", "ok");

    /* A missing variable is a valid default, not an environment read failure. */
    char value[128];
    assert(env_get("missing", value, sizeof(value)) == 0 && !value[0]);
    assert(unlink(envfile) == 0);
    assert(!apply_boot_desired(ctx));
    field(ctx, "boot:dbc", "theme", "librescoot-xp");

    redisFree(ctx);
    assert(kill(pid, SIGTERM) == 0);
    assert(waitpid(pid, NULL, 0) == pid);
    unlink(sock);
    assert(unlink(printenv) == 0 && unlink(setenv_path) == 0 && rmdir(dir) == 0);
    return 0;
}
