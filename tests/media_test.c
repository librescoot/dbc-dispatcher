#define main dispatcher_main
#include "../src/main.c"
#undef main
#include <assert.h>

int main(void) {
    assert(!strcmp(media_ext("/data/sample.MP4"), "mp4"));
    assert(!strcmp(media_ext("http://192.168.7.1:8080/a.jpeg?download=1"), "jpg"));
    assert(!strcmp(media_ext("https://example.org/a.png#fragment"), "png"));
    assert(media_ext("/data/a.txt") == NULL);
    assert(media_ext("/data/a.mp4/other") == NULL);
    assert(media_ext("https://example.org/a.mp4.exe") == NULL);

    char source[] = "/tmp/dbc-media-source-XXXXXX";
    char target[] = "/tmp/dbc-media-target-XXXXXX";
    int fd = mkstemp(source);
    assert(fd >= 0 && write(fd, "media", 5) == 5);
    close(fd);
    fd = mkstemp(target);
    assert(fd >= 0);
    close(fd);
    assert(copy_media(source, target) == 0);
    char result[8] = {0};
    fd = open(target, O_RDONLY);
    assert(fd >= 0 && read(fd, result, sizeof(result)) == 5);
    close(fd);
    assert(!strcmp(result, "media"));
    assert(copy_media("/tmp", target) == -1);
    assert(truncate(source, 0) == 0);
    assert(copy_media(source, target) == -1);
    unlink(source);
    unlink(target);

    char directory[] = "/tmp/dbc-media-cache-XXXXXX";
    assert(mkdtemp(directory));
    char paths[10][PATH_MAX];
    for (int i = 0; i < 10; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/%016x.mp4", directory, i);
        fd = open(paths[i], O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(fd >= 0 && write(fd, "x", 1) == 1);
        close(fd);
        struct timespec times[] = {{100 + i, 0}, {100 + i, 0}};
        assert(utimensat(AT_FDCWD, paths[i], times, 0) == 0);
    }
    media_prune(directory, paths[9]);
    assert(access(paths[0], F_OK) != 0 && access(paths[1], F_OK) != 0);
    for (int i = 2; i < 10; i++) assert(access(paths[i], F_OK) == 0);
    media_clear_cache(directory, paths[9]);
    for (int i = 0; i < 9; i++) assert(access(paths[i], F_OK) != 0);
    assert(access(paths[9], F_OK) == 0);
    char partial[PATH_MAX];
    snprintf(partial, sizeof(partial), "%s/incoming.part", directory);
    fd = open(partial, O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0);
    close(fd);
    media_clear_cache(directory, NULL);
    assert(access(paths[9], F_OK) != 0 && access(partial, F_OK) != 0);

    for (int i = 0; i < 3; i++) {
        fd = open(paths[i], O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(fd >= 0 && ftruncate(fd, 90 * 1024 * 1024) == 0);
        close(fd);
        struct timespec times[] = {{100 + i, 0}, {100 + i, 0}};
        assert(utimensat(AT_FDCWD, paths[i], times, 0) == 0);
    }
    media_prune(directory, paths[2]);
    assert(access(paths[0], F_OK) != 0);
    assert(access(paths[1], F_OK) == 0 && access(paths[2], F_OK) == 0);
    media_clear_cache(directory, NULL);
    assert(rmdir(directory) == 0);
    return 0;
}
