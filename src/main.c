#define _GNU_SOURCE
#include <errno.h>
#include <hiredis/hiredis.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define DEFAULT_APP      "scootui-qt"
#define REDIS_HOST       "192.168.7.1"
#define REDIS_PORT       6379
#define REDIS_KEY        "settings"
#define REDIS_FIELD      "dashboard.app"
#define SETTINGS_CHANNEL "settings"
#define COMMAND_CHANNEL  "dbc:command"
#define UNIT_SUFFIX      ".service"
#define STOP_TIMEOUT_US   5000000
#define CACHE_DIR         "/var/lib/dbc-dispatcher"
#define CACHE_FILE        CACHE_DIR "/last-app"

static bool use_journal = false;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    if (!use_journal) {
        struct timespec ts;
        struct tm tm;
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm);
        fprintf(stderr, "%04d/%02d/%02d %02d:%02d:%02d.%06ld ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void unit_name(const char *app, char *buf, size_t bufsz) {
    size_t len = strlen(app);
    size_t sfxlen = strlen(UNIT_SUFFIX);
    if (len >= sfxlen && strcmp(app + len - sfxlen, UNIT_SUFFIX) == 0)
        snprintf(buf, bufsz, "%s", app);
    else
        snprintf(buf, bufsz, "%s%s", app, UNIT_SUFFIX);
}

static int wait_for_job(sd_bus *bus, const char *job_path, const char *unit,
                        const char *action) {
    int r;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += STOP_TIMEOUT_US / 1000000;

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            log_msg("%s %s: timed out waiting for job", action, unit);
            return -1;
        }

        sd_bus_message *msg = NULL;
        r = sd_bus_process(bus, &msg);
        if (r < 0) {
            log_msg("%s %s: bus process error: %s", action, unit, strerror(-r));
            return r;
        }

        if (msg) {
            if (sd_bus_message_is_signal(msg, "org.freedesktop.systemd1.Manager",
                                         "JobRemoved")) {
                uint32_t id;
                const char *path = NULL, *job_unit = NULL, *result = NULL;
                r = sd_bus_message_read(msg, "uoss", &id, &path, &job_unit,
                                        &result);
                if (r >= 0 && strcmp(path, job_path) == 0) {
                    sd_bus_message_unref(msg);
                    if (strcmp(result, "done") != 0) {
                        log_msg("%s %s: job result %s", action, unit, result);
                        return -1;
                    }
                    return 0;
                }
            }
            sd_bus_message_unref(msg);
            continue;
        }

        uint64_t remaining_us = (deadline.tv_sec - now.tv_sec) * 1000000 +
                                (deadline.tv_nsec - now.tv_nsec) / 1000;
        if (remaining_us > STOP_TIMEOUT_US)
            remaining_us = STOP_TIMEOUT_US;
        r = sd_bus_wait(bus, remaining_us);
        if (r < 0) {
            log_msg("%s %s: bus wait error: %s", action, unit, strerror(-r));
            return r;
        }
    }
}

static int start_unit(sd_bus *bus, const char *unit) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    log_msg("starting %s", unit);

    r = sd_bus_call_method(bus, "org.freedesktop.systemd1",
                           "/org/freedesktop/systemd1",
                           "org.freedesktop.systemd1.Manager", "StartUnit",
                           &error, &reply, "ss", unit, "replace");
    if (r < 0) {
        log_msg("start %s: %s", unit, error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return r;
    }

    const char *job_path = NULL;
    r = sd_bus_message_read(reply, "o", &job_path);
    if (r < 0) {
        log_msg("start %s: failed to read job path: %s", unit, strerror(-r));
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return r;
    }

    char job_path_copy[256];
    snprintf(job_path_copy, sizeof(job_path_copy), "%s", job_path);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    r = wait_for_job(bus, job_path_copy, unit, "start");
    if (r < 0)
        return r;

    log_msg("started %s", unit);
    return 0;
}

static void stop_unit(sd_bus *bus, const char *unit) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    log_msg("stopping %s", unit);

    r = sd_bus_call_method(bus, "org.freedesktop.systemd1",
                           "/org/freedesktop/systemd1",
                           "org.freedesktop.systemd1.Manager", "StopUnit",
                           &error, &reply, "ss", unit, "replace");
    if (r < 0) {
        log_msg("stop %s: %s", unit, error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        return;
    }

    const char *job_path = NULL;
    r = sd_bus_message_read(reply, "o", &job_path);
    if (r < 0) {
        log_msg("stop %s: failed to read job path: %s", unit, strerror(-r));
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return;
    }

    char job_path_copy[256];
    snprintf(job_path_copy, sizeof(job_path_copy), "%s", job_path);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    r = wait_for_job(bus, job_path_copy, unit, "stop");
    if (r == 0)
        log_msg("stopped %s", unit);
    else
        log_msg("stop %s: timed out or failed", unit);
}

/* Returns 0 on success (buf holds the configured app, or DEFAULT_APP when the
 * field is unset), -1 when Redis couldn't be read — callers must not treat
 * that as "use the default", the connection is broken. */
static int read_setting(redisContext *ctx, char *buf, size_t bufsz) {
    if (!ctx)
        return -1;

    redisReply *reply = redisCommand(ctx, "HGET %s %s", REDIS_KEY, REDIS_FIELD);
    if (!reply) {
        log_msg("redis read error: %s", ctx->errstr);
        return -1;
    }

    if (reply->type == REDIS_REPLY_STRING && reply->len > 0)
        snprintf(buf, bufsz, "%s", reply->str);
    else
        snprintf(buf, bufsz, "%s", DEFAULT_APP);

    freeReplyObject(reply);
    return 0;
}

static void do_poweroff(void) {
    log_msg("executing poweroff");
    pid_t pid = fork();
    if (pid == 0) {
        execlp("poweroff", "poweroff", NULL);
        _exit(127);
    } else if (pid < 0) {
        log_msg("poweroff fork failed: %s", strerror(errno));
    }
}

static char *strip(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}

/* Last app Redis told us to run, persisted so the next boot can start it
 * before the MDB is reachable over usb0. */
static void cache_read(char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "%s", DEFAULT_APP);
    FILE *f = fopen(CACHE_FILE, "r");
    if (!f)
        return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *s = strip(line);
        if (*s)
            snprintf(buf, bufsz, "%s", s);
    }
    fclose(f);
}

/* Persist only after a successful switch, atomically, so early boot can start
 * the last known display before Redis is reachable over usb0. */
static void cache_write(const char *app) {
    mkdir(CACHE_DIR, 0755);
    static const char tmp[] = CACHE_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_msg("cache write failed: %s", strerror(errno));
        return;
    }
    fprintf(f, "%s\n", app);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, CACHE_FILE) != 0)
        log_msg("cache rename failed: %s", strerror(errno));
}

/* Start the replacement only after stopping the old unit; preserve the old
 * selection and restart it if the new unit cannot start. */
static void switch_to(sd_bus *bus, const char *new_app, char *current_unit,
                      size_t unitsz) {
    char new_unit[512];
    unit_name(new_app, new_unit, sizeof(new_unit));

    if (strcmp(new_unit, current_unit) == 0)
        return;

    log_msg("switching %s -> %s", current_unit, new_unit);
    stop_unit(bus, current_unit);
    if (start_unit(bus, new_unit) != 0) {
        log_msg("failed to start %s, reverting to %s", new_unit, current_unit);
        if (start_unit(bus, current_unit) != 0)
            log_msg("revert also failed");
        return;
    }
    snprintf(current_unit, unitsz, "%s", new_unit);
    cache_write(new_app);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("dbc-dispatcher %s\n", VERSION);
        return 0;
    }

    use_journal = getenv("JOURNAL_STREAM") != NULL;

    log_msg("dbc-dispatcher %s starting", VERSION);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        log_msg("signalfd: %s", strerror(errno));
        return 1;
    }

    /* Connect directly to systemd's private bus (no D-Bus daemon needed) */
    sd_bus *bus = NULL;
    int r = sd_bus_new(&bus);
    if (r < 0) {
        log_msg("failed to create bus: %s", strerror(-r));
        return 1;
    }
    r = sd_bus_set_address(bus, "unix:path=/run/systemd/private");
    if (r < 0) {
        log_msg("failed to set bus address: %s", strerror(-r));
        sd_bus_unref(bus);
        return 1;
    }
    r = sd_bus_start(bus);
    if (r < 0) {
        log_msg("failed to connect to systemd: %s", strerror(-r));
        sd_bus_unref(bus);
        return 1;
    }

    r = sd_bus_add_match(bus, NULL,
        "type='signal',"
        "sender='org.freedesktop.systemd1',"
        "interface='org.freedesktop.systemd1.Manager',"
        "member='JobRemoved',"
        "path='/org/freedesktop/systemd1'",
        NULL, NULL);
    if (r < 0) {
        log_msg("failed to subscribe to JobRemoved: %s", strerror(-r));
        sd_bus_unref(bus);
        return 1;
    }

    /* Start the last known app immediately. Redis lives on the MDB and is
     * only reachable once usb0 networking is up, seconds from now; the poll
     * loop below reconciles against the actual setting when it can. */
    char app_name[256];
    cache_read(app_name, sizeof(app_name));
    log_msg("app=%s (cached)", app_name);

    char current_unit[512];
    unit_name(app_name, current_unit, sizeof(current_unit));

    if (start_unit(bus, current_unit) != 0) {
        char fallback[512];
        unit_name(DEFAULT_APP, fallback, sizeof(fallback));
        if (strcmp(fallback, current_unit) == 0) {
            log_msg("failed to start %s, no fallback available", current_unit);
            sd_bus_unref(bus);
            return 1;
        }
        log_msg("falling back to %s", fallback);
        snprintf(current_unit, sizeof(current_unit), "%s", fallback);
        if (start_unit(bus, current_unit) != 0) {
            log_msg("failed to start fallback %s", current_unit);
            sd_bus_unref(bus);
            return 1;
        }
    }

    redisContext *rctx = NULL;
    redisContext *sub_ctx = NULL;
    bool synced = false;
    bool shutting_down = false;
    int sub_fd = -1;

    for (;;) {
        struct pollfd fds[2];
        int nfds = 0;

        fds[nfds].fd = sfd;
        fds[nfds].events = POLLIN;
        nfds++;

        if (sub_fd >= 0) {
            fds[nfds].fd = sub_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int ret = poll(fds, nfds, synced ? -1 : 1000);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            log_msg("poll: %s", strerror(errno));
            break;
        }

        /* Not yet synced with Redis: (re)establish and reconcile. Subscribe
         * BEFORE reading the setting so a change can't slip between the two. */
        if (ret == 0) {
            struct timeval tv = {0, 500000};
            if (!sub_ctx) {
                sub_ctx = redisConnectWithTimeout(REDIS_HOST, REDIS_PORT, tv);
                if (!sub_ctx || sub_ctx->err) {
                    if (sub_ctx) {
                        redisFree(sub_ctx);
                        sub_ctx = NULL;
                    }
                    continue;
                }
                redisReply *r2 = redisCommand(sub_ctx, "SUBSCRIBE %s %s",
                                              SETTINGS_CHANNEL, COMMAND_CHANNEL);
                if (r2) freeReplyObject(r2);
                sub_fd = sub_ctx->fd;
                log_msg("watching %s and %s channels", SETTINGS_CHANNEL,
                        COMMAND_CHANNEL);
            }
            if (!rctx) {
                rctx = redisConnectWithTimeout(REDIS_HOST, REDIS_PORT, tv);
                if (rctx && rctx->err) {
                    redisFree(rctx);
                    rctx = NULL;
                }
            }
            if (rctx) {
                char cfg_app[256];
                if (read_setting(rctx, cfg_app, sizeof(cfg_app)) == 0) {
                    switch_to(bus, cfg_app, current_unit, sizeof(current_unit));
                    synced = true;
                    log_msg("synced with redis, app=%s", cfg_app);
                } else {
                    redisFree(rctx);
                    rctx = NULL;
                }
            }
            continue;
        }

        if (fds[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                log_msg("shutting down, stopping %s", current_unit);
                stop_unit(bus, current_unit);
                break;
            }
        }

        if (nfds > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            redisReply *reply = NULL;
            if (redisGetReply(sub_ctx, (void **)&reply) != REDIS_OK || !reply) {
                log_msg("redis subscription lost, resyncing...");
                redisFree(sub_ctx);
                sub_ctx = NULL;
                sub_fd = -1;
                if (rctx) {
                    redisFree(rctx);
                    rctx = NULL;
                }
                /* Reconcile after reconnecting so changes made while disconnected survive. */
                synced = false;
                continue;
            }

            if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 3 &&
                reply->element[0]->str &&
                reply->element[1]->str &&
                reply->element[2]->str &&
                strcmp(reply->element[0]->str, "message") == 0) {

                const char *channel = reply->element[1]->str;
                char *payload_raw = strdup(reply->element[2]->str);
                char *payload = strip(payload_raw);

                if (strcmp(channel, COMMAND_CHANNEL) == 0) {
                    if (strcmp(payload, "poweroff") == 0) {
                        if (!shutting_down) {
                            shutting_down = true;
                            log_msg("received poweroff command, executing poweroff");
                            free(payload_raw);
                            freeReplyObject(reply);
                            /* Don't serialize on stopping the display unit first
                             * — shutdown.target will SIGTERM it in parallel with
                             * everything else. VBUS is cut 5s after the vehicle
                             * FSM enters ShuttingDown, so we want to burn as
                             * little of that budget as possible before handing
                             * off to systemd. */
                            do_poweroff();
                            goto done;
                        } else {
                            log_msg("poweroff already in progress, ignoring");
                        }
                    } else {
                        log_msg("unknown command: %s", payload);
                    }
                } else if (strcmp(channel, SETTINGS_CHANNEL) == 0) {
                    if (strcmp(payload, REDIS_FIELD) == 0) {
                        char new_app[256];
                        if (read_setting(rctx, new_app, sizeof(new_app)) == 0) {
                            log_msg("setting %s changed, new value: %s",
                                    REDIS_FIELD, new_app);
                            switch_to(bus, new_app, current_unit,
                                      sizeof(current_unit));
                        } else {
                            /* Reconnect and reconcile rather than dropping this setting change. */
                            if (rctx) {
                                redisFree(rctx);
                                rctx = NULL;
                            }
                            synced = false;
                        }
                    }
                }

                free(payload_raw);
            }

            freeReplyObject(reply);
        }
    }

done:
    close(sfd);
    sd_bus_unref(bus);
    if (rctx) redisFree(rctx);
    if (sub_ctx) redisFree(sub_ctx);
    return 0;
}
