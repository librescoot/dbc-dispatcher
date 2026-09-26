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
    return 0;
}
