#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <hiredis/hiredis.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/wait.h>
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
#define LOGSERVER_FIELD  "scooter.logserver"
#define JOURNAL_UNIT     "systemd-journal-upload.service"
#define JOURNAL_CONF     "/etc/systemd/journal-upload.conf"
#define UNIT_SUFFIX      ".service"
#define STOP_TIMEOUT_US   5000000
#define CACHE_DIR         "/var/lib/dbc-dispatcher"
#define CACHE_FILE        CACHE_DIR "/last-app"
#define MEDIA_DIR         "/data/dbc-dispatcher"
#define MEDIA_LIMIT       (100 * 1024 * 1024)
#define MEDIA_CACHE_BYTES (256 * 1024 * 1024)
#define MEDIA_CACHE_ITEMS 8
#define MEDIA_URL_MAX     2048

/* The boot logo, animation and startup sound are chosen by U-Boot from its own
 * environment, which only something running on the DBC can write. A request
 * therefore arrives as desired state in Redis and is reconciled here: on every
 * start, and immediately when a live request arrives. Desired state rather
 * than a queue, so it converges to the latest value and cannot replay a stale
 * request; a field rather than a channel, so a request made while the DBC is
 * powered off is still there when it next starts. */
#define ANIM_DIR          "/usr/share/boot-animation"
#define BOOT_DESIRED_KEY  "boot:desired"
#define BOOT_STATE_KEY    "boot:dbc"
#define BOOT_CHANNEL      "boot:command"
#define BOOT_THEME_VAR    "boot_animation"
#define BOOT_SOUND_VAR    "boot_sound"
#define BOOT_DEFAULT_THEME "librescoot"

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

static int stop_unit(sd_bus *bus, const char *unit) {
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
        return r;
    }

    const char *job_path = NULL;
    r = sd_bus_message_read(reply, "o", &job_path);
    if (r < 0) {
        log_msg("stop %s: failed to read job path: %s", unit, strerror(-r));
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return r;
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
    return r;
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
    if (stop_unit(bus, current_unit) != 0)
        return;
    if (start_unit(bus, new_unit) != 0) {
        log_msg("failed to start %s, reverting to %s", new_unit, current_unit);
        if (start_unit(bus, current_unit) != 0)
            log_msg("revert also failed");
        return;
    }
    snprintf(current_unit, unitsz, "%s", new_unit);
    cache_write(new_app);
}

/* --- temporary mock-up display ------------------------------------------ */

typedef struct {
    pid_t fetch;
    pid_t player;
    char staged[PATH_MAX];
    char cached[PATH_MAX];
    char next[PATH_MAX];
    bool video;
} Media;

static const char *media_ext(const char *source) {
    const char *end = source + strcspn(source, "?#");
    const char *dot = NULL;
    for (const char *p = source; p < end; p++) {
        if (*p == '/') dot = NULL;
        else if (*p == '.') dot = p;
    }
    if (!dot) return NULL;
    if (end - dot == 4 && !strncasecmp(dot, ".mp4", 4)) return "mp4";
    if (end - dot == 4 && !strncasecmp(dot, ".jpg", 4)) return "jpg";
    if (end - dot == 5 && !strncasecmp(dot, ".jpeg", 5)) return "jpg";
    if (end - dot == 4 && !strncasecmp(dot, ".png", 4)) return "png";
    return NULL;
}

static void kill_child(pid_t *pid) {
    if (*pid <= 0) return;
    kill(*pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (waitpid(*pid, NULL, WNOHANG) == *pid) {
            *pid = 0;
            return;
        }
        usleep(50000);
    }
    kill(*pid, SIGKILL);
    waitpid(*pid, NULL, 0);
    *pid = 0;
}

static void child_signals(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

static int copy_media(const char *source, const char *target) {
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > MEDIA_LIMIT) {
        close(in);
        return -1;
    }
    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int ok = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        char *p = buf;
        while (n > 0) {
            ssize_t written = write(out, p, n);
            if (written <= 0) { ok = -1; break; }
            n -= written;
            p += written;
        }
        if (ok) break;
    }
    if (n < 0 || fsync(out) != 0) ok = -1;
    close(out);
    close(in);
    return ok;
}

static bool cache_name(const char *name) {
    size_t len = strlen(name);
    if (len != 20 && len != 21) return false;
    for (int i = 0; i < 16; i++)
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return !strcmp(name + 16, ".mp4") || !strcmp(name + 16, ".jpg") ||
           !strcmp(name + 16, ".png");
}

static void cache_path(const char *source, const char *ext, char *path, size_t size) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)source; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    snprintf(path, size, MEDIA_DIR "/%016llx.%s", (unsigned long long)hash, ext);
}

static void media_clear_cache(const char *directory, const char *keep) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!cache_name(entry->d_name)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (!keep || strcmp(path, keep)) unlink(path);
    }
    closedir(dir);
    char staged[PATH_MAX];
    snprintf(staged, sizeof(staged), "%s/incoming.part", directory);
    unlink(staged);
    const char *legacy[] = {"mp4", "jpg", "png"};
    for (size_t i = 0; i < 3; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/media.%s", directory, legacy[i]);
        unlink(path);
    }
}

static void media_prune(const char *directory, const char *keep) {
    /* The most recently played item survives even when it alone exceeds the budget. */
    for (;;) {
        DIR *dir = opendir(directory);
        if (!dir) return;
        struct dirent *entry;
        off_t bytes = 0;
        int count = 0;
        char oldest[PATH_MAX] = "";
        struct timespec oldest_time = {0};
        while ((entry = readdir(dir))) {
            if (!cache_name(entry->d_name)) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            struct stat st;
            if (lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
            bytes += st.st_size;
            count++;
            if (keep && !strcmp(path, keep)) continue;
            if (!oldest[0] || st.st_mtim.tv_sec < oldest_time.tv_sec ||
                (st.st_mtim.tv_sec == oldest_time.tv_sec &&
                 st.st_mtim.tv_nsec < oldest_time.tv_nsec)) {
                snprintf(oldest, sizeof(oldest), "%s", path);
                oldest_time = st.st_mtim;
            }
        }
        closedir(dir);
        if (count <= MEDIA_CACHE_ITEMS && bytes <= MEDIA_CACHE_BYTES) return;
        if (!oldest[0]) return;
        if (unlink(oldest) != 0) {
            log_msg("media: cache eviction failed: %s", strerror(errno));
            return;
        }
    }
}

static void media_cancel(Media *m, sd_bus *bus, const char *unit, bool restore) {
    kill_child(&m->fetch);
    if (m->staged[0]) unlink(m->staged);
    m->staged[0] = '\0';
    m->next[0] = '\0';
    kill_child(&m->player);
    m->cached[0] = '\0';
    if (restore && start_unit(bus, unit) != 0)
        log_msg("media: failed to restore %s", unit);
}

static void media_start(Media *m, sd_bus *bus, const char *unit, bool refreshed) {
    if (!refreshed && m->player > 0 && !strcmp(m->cached, m->next)) {
        utimensat(AT_FDCWD, m->cached, NULL, 0);
        media_prune(MEDIA_DIR, m->cached);
        return;
    }
    bool had_player = m->player > 0;
    kill_child(&m->player);
    if (!had_player && stop_unit(bus, unit) != 0) {
        log_msg("media: could not stop %s", unit);
        return;
    }
    snprintf(m->cached, sizeof(m->cached), "%s", m->next);
    pid_t pid = fork();
    if (pid == 0) {
        child_signals();
        if (getppid() == 1) _exit(1);
        if (m->video)
            execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "warning",
                   "-nostdin", "-re", "-stream_loop", "-1", "-i", m->cached,
                   "-an", "-vf", "scale=480:480:force_original_aspect_ratio=decrease,"
                   "pad=480:480:(ow-iw)/2:(oh-ih)/2", "-pix_fmt", "bgra",
                   "-f", "fbdev", "/dev/fb0", (char *)NULL);
        else
            execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "warning",
                   "-nostdin", "-loop", "1", "-framerate", "1", "-re",
                   "-i", m->cached, "-vf", "scale=480:480:force_original_aspect_ratio=decrease,"
                   "pad=480:480:(ow-iw)/2:(oh-ih)/2", "-pix_fmt", "bgra",
                   "-f", "fbdev", "/dev/fb0", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        log_msg("media: player fork: %s", strerror(errno));
        m->cached[0] = '\0';
        start_unit(bus, unit);
        return;
    }
    m->player = pid;
    utimensat(AT_FDCWD, m->cached, NULL, 0);
    media_prune(MEDIA_DIR, m->cached);
    log_msg("media: displaying %s (pid %d)", m->cached, (int)pid);
}

static void media_request(Media *m, sd_bus *bus, const char *unit, const char *source) {
    size_t length = strlen(source);
    if (!length || length >= MEDIA_URL_MAX || strchr(source, '\n') || strchr(source, '\r')) {
        log_msg("media: invalid source length or characters");
        return;
    }
    bool remote = !strncmp(source, "http://", 7) || !strncmp(source, "https://", 8);
    const char *ext = media_ext(source);
    if ((!remote && source[0] != '/') || !ext) {
        log_msg("media: expected absolute local path or HTTP(S) URL ending in mp4, jpg, jpeg or png");
        return;
    }
    if (mkdir(MEDIA_DIR, 0755) != 0 && errno != EEXIST) {
        log_msg("media: mkdir: %s", strerror(errno));
        return;
    }
    kill_child(&m->fetch);
    if (m->staged[0]) unlink(m->staged);
    m->staged[0] = '\0';
    cache_path(source, ext, m->next, sizeof(m->next));
    m->video = strcmp(ext, "mp4") == 0;
    struct stat cached, original;
    if (!lstat(m->next, &cached) && S_ISREG(cached.st_mode) &&
        cached.st_size > 0 && cached.st_size <= MEDIA_LIMIT &&
        (remote || (stat(source, &original) == 0 &&
                    original.st_mtim.tv_sec <= cached.st_mtim.tv_sec &&
                    (original.st_mtim.tv_sec < cached.st_mtim.tv_sec ||
                     original.st_mtim.tv_nsec <= cached.st_mtim.tv_nsec)))) {
        log_msg("media: cache hit for %s", source);
        media_start(m, bus, unit, false);
        return;
    }
    snprintf(m->staged, sizeof(m->staged), MEDIA_DIR "/incoming.part");
    pid_t pid = fork();
    if (pid < 0) {
        log_msg("media: fork: %s", strerror(errno));
        m->staged[0] = '\0';
        return;
    }
    if (pid == 0) {
        child_signals();
        if (getppid() == 1) _exit(1);
        struct rlimit limit = {MEDIA_LIMIT, MEDIA_LIMIT};
        setrlimit(RLIMIT_FSIZE, &limit);
        if (!remote) _exit(copy_media(source, m->staged) == 0 ? 0 : 1);
        execlp("curl", "curl", "--fail", "--silent", "--show-error", "--location",
               "--max-redirs", "3", "--max-time", "120", "--max-filesize", "104857600",
               "--proto", "=http,https", "--proto-redir", "=http,https",
               "--output", m->staged, "--", source, (char *)NULL);
        _exit(127);
    }
    m->fetch = pid;
    log_msg("media: caching %s", source);
}

static void media_poll(Media *m, sd_bus *bus, const char *unit) {
    int status;
    if (m->fetch > 0 && waitpid(m->fetch, &status, WNOHANG) == m->fetch) {
        m->fetch = 0;
        struct stat st;
        if (!WIFEXITED(status) || WEXITSTATUS(status) || stat(m->staged, &st) ||
            !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > MEDIA_LIMIT) {
            log_msg("media: download/copy failed; leaving display unchanged");
            unlink(m->staged);
            m->staged[0] = '\0';
            return;
        }
        if (rename(m->staged, m->next) != 0) {
            log_msg("media: cache rename: %s", strerror(errno));
            unlink(m->staged);
            m->staged[0] = '\0';
            return;
        }
        m->staged[0] = '\0';
        media_start(m, bus, unit, true);
    }
    if (m->player > 0 && waitpid(m->player, &status, WNOHANG) == m->player) {
        log_msg("media: player exited (status %d), restoring %s", status, unit);
        m->player = 0;
        media_cancel(m, bus, unit, true);
    }
}

/* --- journal upload ------------------------------------------------------ */

static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int read_logserver(redisContext *ctx, char *buf, size_t bufsz) {
    if (!ctx)
        return -1;
    buf[0] = '\0';
    redisReply *reply = redisCommand(ctx, "HGET %s %s", REDIS_KEY, LOGSERVER_FIELD);
    if (!reply) {
        log_msg("redis read error: %s", ctx->errstr);
        return -1;
    }
    if (reply->type == REDIS_REPLY_STRING)
        snprintf(buf, bufsz, "%s", reply->str);
    freeReplyObject(reply);
    return 0;
}

/* URL currently in the conf's [Upload] section; empty when absent. */
static void conf_url(char *buf, size_t bufsz) {
    buf[0] = '\0';
    FILE *f = fopen(JOURNAL_CONF, "r");
    if (!f)
        return;
    char line[1024];
    int in_upload = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = strip(line);
        if (*s == '#' || !*s)
            continue;
        if (*s == '[') {
            in_upload = strcmp(s, "[Upload]") == 0;
            continue;
        }
        if (in_upload && strncmp(s, "URL=", 4) == 0) {
            snprintf(buf, bufsz, "%s", s + 4);
            break;
        }
    }
    fclose(f);
}

/* Same content settings-service writes on the MDB, replaced atomically. */
static int conf_write(const char *url) {
    static const char tmp[] = JOURNAL_CONF ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_msg("journal-upload conf write failed: %s", strerror(errno));
        return -1;
    }
    fprintf(f, "[Upload]\nURL=%s\nServerKeyFile=-\nServerCertificateFile=-\n"
               "TrustedCertificateFile=-\n", url);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, JOURNAL_CONF) != 0) {
        log_msg("journal-upload conf rename failed: %s", strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

/* scooter.logserver drives journal-upload on this board the way
 * settings-service drives it on the MDB: unset stops and disables it, set
 * rewrites the conf and enables/restarts it. Returns -1 only when the read
 * from Redis failed, so callers can resync instead of dropping the change. */
static int apply_logserver(redisContext *ctx) {
    char url[512];
    char current[512];

    if (read_logserver(ctx, url, sizeof(url)) != 0)
        return -1;

    /* The URL lands in the conf verbatim; a newline would corrupt it. */
    if (strchr(url, '\n') || strchr(url, '\r')) {
        log_msg("log server: refusing URL containing a newline");
        url[0] = '\0';
    }

    if (!url[0]) {
        if (run_cmd((char *[]){"systemctl", "disable", "--now", JOURNAL_UNIT, NULL}) == 0)
            log_msg("log server unset, journal-upload stopped and disabled");
        else
            log_msg("failed to stop/disable %s", JOURNAL_UNIT);
        return 0;
    }

    conf_url(current, sizeof(current));
    bool changed = strcmp(current, url) != 0;
    if (changed && conf_write(url) != 0)
        return 0;

    bool active = run_cmd((char *[]){"systemctl", "is-active", "--quiet", JOURNAL_UNIT, NULL}) == 0;
    if (changed || !active) {
        if (run_cmd((char *[]){"systemctl", "enable", JOURNAL_UNIT, NULL}) != 0 ||
            run_cmd((char *[]){"systemctl", "restart", JOURNAL_UNIT, NULL}) != 0) {
            log_msg("failed to enable/restart %s", JOURNAL_UNIT);
            return 0;
        }
        log_msg("log server %s, journal-upload enabled", url);
    }
    return 0;
}

/* --- boot theme and sound ------------------------------------------------ */

/* A theme name reaches fw_setenv verbatim, so keep it to a shape a theme name
 * can take and nothing else. */
static int valid_theme_name(const char *name) {
    if (!name || !*name)
        return 0;
    for (const char *p = name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

static int theme_installed(const char *name) {
    char path[512];
    if (!valid_theme_name(name))
        return 0;
    snprintf(path, sizeof(path), ANIM_DIR "/%s.json", name);
    return access(path, R_OK) == 0;
}

/* Read one variable from the U-Boot environment. An unset variable is not an
 * error: it leaves the buffer empty, which every caller treats as a default.
 * Returns -1 only when fw_printenv itself could not run. */
static int env_get(const char *key, char *buf, size_t bufsz) {
    char cmd[256];
    if (bufsz == 0)
        return -1;
    buf[0] = '\0';
    snprintf(cmd, sizeof(cmd), "fw_printenv -n %s 2>/dev/null", key);
    FILE *f = popen(cmd, "r");
    if (!f)
        return -1;
    if (fgets(buf, (int)bufsz, f)) {
        char *s = strip(buf);
        if (s != buf)
            memmove(buf, s, strlen(s) + 1);
    }
    int status = pclose(f);
    if (status == -1 || !WIFEXITED(status))
        return -1;
    return 0;
}

static int env_set(const char *key, const char *value) {
    pid_t pid = fork();
    if (pid < 0) {
        log_msg("fw_setenv fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execlp("fw_setenv", "fw_setenv", key, value, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Publish what the environment actually holds, so a caller can read the
 * current theme without reaching this board. */
static void publish_boot_state(redisContext *ctx, const char *note) {
    if (!ctx)
        return;
    char theme[128];
    char sound[16];
    if (env_get(BOOT_THEME_VAR, theme, sizeof(theme)) != 0 || !theme[0])
        snprintf(theme, sizeof(theme), "%s", BOOT_DEFAULT_THEME);
    if (env_get(BOOT_SOUND_VAR, sound, sizeof(sound)) != 0 || !sound[0])
        snprintf(sound, sizeof(sound), "1");

    /* redisCommand splits its formatted string on whitespace, so every value
     * here is a single token by construction. */
    redisReply *r = redisCommand(ctx,
        "HSET %s theme %s sound %s note %s updated %ld",
        BOOT_STATE_KEY, theme, sound, note ? note : "ok", (long)time(NULL));
    if (r)
        freeReplyObject(r);
}

static void desired_field(redisContext *ctx, const char *field, char *buf, size_t bufsz) {
    buf[0] = '\0';
    redisReply *r = redisCommand(ctx, "HGET %s %s", BOOT_DESIRED_KEY, field);
    if (r) {
        if (r->type == REDIS_REPLY_STRING && r->len > 0)
            snprintf(buf, bufsz, "%s", r->str);
        freeReplyObject(r);
    }
}

/* Make the environment match boot:desired. Runs at every start and after
 * every live request, so it has to be safe to repeat. */
static void apply_boot_desired(redisContext *ctx) {
    if (!ctx)
        return;

    char theme[128];
    char sound[16];
    char current[128];
    char note[32] = "ok";

    desired_field(ctx, "theme", theme, sizeof(theme));
    desired_field(ctx, "sound", sound, sizeof(sound));

    if (theme[0]) {
        if (!theme_installed(theme)) {
            /* Refuse rather than write a name nothing can boot: the fallback
             * is silent, and the caller would have no idea. */
            snprintf(note, sizeof(note), "unknown-theme");
            log_msg("boot: refusing unknown theme %s", theme);
        } else if (env_get(BOOT_THEME_VAR, current, sizeof(current)) != 0) {
            snprintf(note, sizeof(note), "env-error");
        } else if (strcmp(current, theme) != 0) {
            if (env_set(BOOT_THEME_VAR, theme) == 0)
                log_msg("boot: theme -> %s", theme);
            else
                snprintf(note, sizeof(note), "env-error");
        }
    }

    if (sound[0]) {
        const char *value = NULL;
        if (!strcasecmp(sound, "1") || !strcasecmp(sound, "on") || !strcasecmp(sound, "true"))
            value = "1";
        else if (!strcasecmp(sound, "0") || !strcasecmp(sound, "off") || !strcasecmp(sound, "false"))
            value = "0";

        if (!value) {
            snprintf(note, sizeof(note), "bad-sound");
        } else if (env_get(BOOT_SOUND_VAR, current, sizeof(current)) != 0) {
            snprintf(note, sizeof(note), "env-error");
        } else if (strcmp(current, value) != 0) {
            if (env_set(BOOT_SOUND_VAR, value) == 0)
                log_msg("boot: sound -> %s", value);
            else
                snprintf(note, sizeof(note), "env-error");
        }
    }

    publish_boot_state(ctx, note);
}

/* Handle "boot-theme <name>" and "boot-sound <on|off>". The request is written
 * to boot:desired first, so it still applies if this process is restarted
 * before the next boot. */
static void handle_boot_request(redisContext *ctx, char *payload) {
    if (!ctx) {
        log_msg("boot: request dropped, redis is down");
        return;
    }
    char *sp = strchr(payload, ' ');
    if (!sp) {
        log_msg("boot: malformed request '%s'", payload);
        return;
    }
    *sp = '\0';
    char *value = strip(sp + 1);

    const char *field = strcmp(payload, "boot-theme") == 0 ? "theme"
                      : strcmp(payload, "boot-sound") == 0 ? "sound"
                      : NULL;
    if (!field || !*value) {
        log_msg("boot: unknown request '%s'", payload);
        return;
    }

    redisReply *r = redisCommand(ctx, "HSET %s %s %s", BOOT_DESIRED_KEY, field, value);
    if (r)
        freeReplyObject(r);
    apply_boot_desired(ctx);
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

    Media media = {0};
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

        int ret = poll(fds, nfds, (media.fetch > 0 || media.player > 0) ? 250 :
                       (synced ? -1 : 1000));
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            log_msg("poll: %s", strerror(errno));
            break;
        }

        media_poll(&media, bus, current_unit);

        /* Not yet synced with Redis: (re)establish and reconcile. Subscribe
         * BEFORE reading the setting so a change can't slip between the two. */
        if (ret == 0 && !synced) {
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
                redisReply *r2 = redisCommand(sub_ctx, "SUBSCRIBE %s %s %s",
                                              SETTINGS_CHANNEL, COMMAND_CHANNEL,
                                              BOOT_CHANNEL);
                if (r2) freeReplyObject(r2);
                sub_fd = sub_ctx->fd;
                log_msg("watching %s, %s and %s channels", SETTINGS_CHANNEL,
                        COMMAND_CHANNEL, BOOT_CHANNEL);
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
                    if (media.player > 0) {
                        char desired[512];
                        unit_name(cfg_app, desired, sizeof(desired));
                        if (strcmp(desired, current_unit)) {
                            snprintf(current_unit, sizeof(current_unit), "%s", desired);
                            cache_write(cfg_app);
                        }
                    } else {
                        switch_to(bus, cfg_app, current_unit, sizeof(current_unit));
                    }
                    /* The DBC's U-Boot environment is only writable from here,
                     * so this is where a theme requested while the scooter was
                     * asleep gets applied. */
                    apply_boot_desired(rctx);
                    apply_logserver(rctx);
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
                log_msg("shutting down, stopping display");
                bool playing = media.player > 0;
                media_cancel(&media, bus, current_unit, false);
                if (!playing) stop_unit(bus, current_unit);
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
                    } else if (!strncmp(payload, "media:play ", 11)) {
                        media_request(&media, bus, current_unit, strip(payload + 11));
                    } else if (strcmp(payload, "media:stop") == 0) {
                        bool playing = media.player > 0;
                        media_cancel(&media, bus, current_unit, playing);
                    } else if (strcmp(payload, "media:clear") == 0) {
                        kill_child(&media.fetch);
                        if (media.staged[0]) unlink(media.staged);
                        media.staged[0] = '\0';
                        media.next[0] = '\0';
                        media_clear_cache(MEDIA_DIR, media.player > 0 ? media.cached : NULL);
                        log_msg("media: cache cleared");
                    } else {
                        log_msg("unknown command: %s", payload);
                    }
                } else if (strcmp(channel, BOOT_CHANNEL) == 0) {
                    handle_boot_request(rctx, payload);
                } else if (strcmp(channel, SETTINGS_CHANNEL) == 0) {
                    if (strcmp(payload, REDIS_FIELD) == 0) {
                        char new_app[256];
                        if (read_setting(rctx, new_app, sizeof(new_app)) == 0) {
                            log_msg("setting %s changed, new value: %s",
                                    REDIS_FIELD, new_app);
                            if (media.player > 0) {
                                char desired[512];
                                unit_name(new_app, desired, sizeof(desired));
                                if (strcmp(desired, current_unit)) {
                                    log_msg("media: will restore %s on stop", desired);
                                    snprintf(current_unit, sizeof(current_unit), "%s", desired);
                                    cache_write(new_app);
                                }
                            } else {
                                switch_to(bus, new_app, current_unit,
                                          sizeof(current_unit));
                            }
                        } else {
                            /* Reconnect and reconcile rather than dropping this setting change. */
                            if (rctx) {
                                redisFree(rctx);
                                rctx = NULL;
                            }
                            synced = false;
                        }
                    } else if (strcmp(payload, LOGSERVER_FIELD) == 0) {
                        if (apply_logserver(rctx) != 0) {
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
