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
#define RETRY_INTERVAL_MS 500
#define TIMEOUT_MS        5000
#define STOP_TIMEOUT_US   5000000

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

/* Build unit name: append ".service" if not already present */
static void unit_name(const char *app, char *buf, size_t bufsz) {
    size_t len = strlen(app);
    size_t sfxlen = strlen(UNIT_SUFFIX);
    if (len >= sfxlen && strcmp(app + len - sfxlen, UNIT_SUFFIX) == 0)
        snprintf(buf, bufsz, "%s", app);
    else
        snprintf(buf, bufsz, "%s%s", app, UNIT_SUFFIX);
}

/* Wait for a systemd job to complete, return 0 on success */
static int wait_for_job(sd_bus *bus, const char *job_path, const char *unit,
                        const char *action) {
    int r;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += STOP_TIMEOUT_US / 1000000;

    /* Process bus messages until we see our job complete or timeout */
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

static redisContext *connect_redis(void) {
    struct timeval tv = {0, RETRY_INTERVAL_MS * 1000};
    int elapsed_ms = 0;

    while (elapsed_ms < TIMEOUT_MS) {
        redisContext *ctx = redisConnectWithTimeout(REDIS_HOST, REDIS_PORT, tv);
        if (ctx && ctx->err == 0)
            return ctx;
        if (ctx)
            redisFree(ctx);
        usleep(RETRY_INTERVAL_MS * 1000);
        elapsed_ms += RETRY_INTERVAL_MS;
    }

    log_msg("redis unreachable after %dms, continuing anyway", TIMEOUT_MS);
    return NULL;
}

static void read_setting(redisContext *ctx, char *buf, size_t bufsz) {
    if (!ctx) {
        snprintf(buf, bufsz, "%s", DEFAULT_APP);
        return;
    }

    redisReply *reply = redisCommand(ctx, "HGET %s %s", REDIS_KEY, REDIS_FIELD);
    if (!reply) {
        log_msg("redis read error: %s, using default", ctx->errstr);
        snprintf(buf, bufsz, "%s", DEFAULT_APP);
        return;
    }

    if (reply->type == REDIS_REPLY_STRING && reply->len > 0)
        snprintf(buf, bufsz, "%s", reply->str);
    else
        snprintf(buf, bufsz, "%s", DEFAULT_APP);

    freeReplyObject(reply);
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

/* Strip leading/trailing whitespace in place */
static char *strip(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("dbc-dispatcher %s\n", VERSION);
        return 0;
    }

    use_journal = getenv("JOURNAL_STREAM") != NULL;

    log_msg("dbc-dispatcher %s starting", VERSION);

    /* Block SIGTERM and SIGINT, use signalfd */
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

    /* Connect to systemd D-Bus */
    sd_bus *bus = NULL;
    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        log_msg("failed to connect to systemd D-Bus: %s", strerror(-r));
        return 1;
    }

    /* Subscribe to JobRemoved signals for start/stop job tracking */
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

    /* Connect to Redis */
    redisContext *rctx = connect_redis();

    /* Read configured app */
    char app_name[256];
    read_setting(rctx, app_name, sizeof(app_name));
    log_msg("app=%s", app_name);

    char current_unit[512];
    unit_name(app_name, current_unit, sizeof(current_unit));

    if (start_unit(bus, current_unit) != 0) {
        char fallback[512];
        unit_name(DEFAULT_APP, fallback, sizeof(fallback));
        if (strcmp(fallback, current_unit) == 0) {
            log_msg("failed to start %s, no fallback available", current_unit);
            sd_bus_unref(bus);
            if (rctx) redisFree(rctx);
            return 1;
        }
        log_msg("falling back to %s", fallback);
        snprintf(current_unit, sizeof(current_unit), "%s", fallback);
        if (start_unit(bus, current_unit) != 0) {
            log_msg("failed to start fallback %s", current_unit);
            sd_bus_unref(bus);
            if (rctx) redisFree(rctx);
            return 1;
        }
    }

    /* Open a separate Redis connection for SUBSCRIBE */
    redisContext *sub_ctx = NULL;
    if (rctx) {
        struct timeval tv = {1, 0};
        sub_ctx = redisConnectWithTimeout(REDIS_HOST, REDIS_PORT, tv);
        if (!sub_ctx || sub_ctx->err) {
            log_msg("redis subscribe connection failed");
            if (sub_ctx) {
                redisFree(sub_ctx);
                sub_ctx = NULL;
            }
        }
    }

    if (sub_ctx) {
        redisReply *reply = redisCommand(sub_ctx, "SUBSCRIBE %s %s",
                                         SETTINGS_CHANNEL, COMMAND_CHANNEL);
        if (reply) freeReplyObject(reply);
        log_msg("watching %s and %s channels", SETTINGS_CHANNEL, COMMAND_CHANNEL);
    } else {
        log_msg("no redis subscription, running without live updates");
    }

    bool shutting_down = false;
    int sub_fd = sub_ctx ? sub_ctx->fd : -1;

    /* Main event loop using poll */
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

        int ret = poll(fds, nfds, -1);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            log_msg("poll: %s", strerror(errno));
            break;
        }

        /* Check for signals */
        if (fds[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                log_msg("shutting down, stopping %s", current_unit);
                stop_unit(bus, current_unit);
                break;
            }
        }

        /* Check for Redis messages or connection errors */
        if (nfds > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            redisReply *reply = NULL;
            if (redisGetReply(sub_ctx, (void **)&reply) != REDIS_OK || !reply) {
                log_msg("redis subscription lost, reconnecting...");
                redisFree(sub_ctx);
                sub_ctx = NULL;
                sub_fd = -1;

                /* Reconnect */
                struct timeval tv = {1, 0};
                sub_ctx = redisConnectWithTimeout(REDIS_HOST, REDIS_PORT, tv);
                if (sub_ctx && !sub_ctx->err) {
                    redisReply *r2 = redisCommand(sub_ctx, "SUBSCRIBE %s %s",
                                                  SETTINGS_CHANNEL,
                                                  COMMAND_CHANNEL);
                    if (r2) freeReplyObject(r2);
                    sub_fd = sub_ctx->fd;
                    log_msg("redis subscription restored");
                } else {
                    if (sub_ctx) {
                        redisFree(sub_ctx);
                        sub_ctx = NULL;
                    }
                    log_msg("redis reconnect failed");
                }
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
                            log_msg("received poweroff command, stopping %s",
                                    current_unit);
                            stop_unit(bus, current_unit);
                            free(payload_raw);
                            freeReplyObject(reply);
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
                        read_setting(rctx, new_app, sizeof(new_app));
                        log_msg("setting %s changed, new value: %s", REDIS_FIELD,
                                new_app);

                        char new_unit[512];
                        unit_name(new_app, new_unit, sizeof(new_unit));

                        if (strcmp(new_unit, current_unit) != 0) {
                            log_msg("switching %s -> %s", current_unit, new_unit);
                            stop_unit(bus, current_unit);

                            if (start_unit(bus, new_unit) != 0) {
                                log_msg("failed to start %s, reverting to %s",
                                        new_unit, current_unit);
                                if (start_unit(bus, current_unit) != 0)
                                    log_msg("revert also failed");
                            } else {
                                snprintf(current_unit, sizeof(current_unit), "%s",
                                         new_unit);
                            }
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
