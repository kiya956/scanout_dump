#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    if (argc < 3) { fprintf(stderr, "usage: %s <card_name> <crtc_id>\n", argv[0]); return 1; }

    char path[64];
    snprintf(path, sizeof(path), "/dev/dri/%s", argv[1]);
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    drmModeCrtc *crtc = drmModeGetCrtc(fd, atoi(argv[2]));
    if (!crtc) { fprintf(stderr, "GetCrtc failed: %s\n", strerror(errno)); return 1; }
    printf("fb_id=%d size=%dx%d\n", crtc->buffer_id, crtc->width, crtc->height);

    drmModeFB *fb = drmModeGetFB(fd, crtc->buffer_id);
    if (!fb) { fprintf(stderr, "GetFB failed: %s\n", strerror(errno)); return 1; }
    printf("fb: %dx%d pitch=%d handle=%d\n", fb->width, fb->height, fb->pitch, fb->handle);

    if (fb->handle == 0) {
        fprintf(stderr, "handle=0, need DRM master or use DRM_IOCTL_MODE_GETFB2\n");
        return 1;
    }

    struct drm_mode_map_dumb map = {};
    map.handle = fb->handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
        perror("MAP_DUMB"); return 1;
    }

    size_t size = fb->pitch * fb->height;
    void *ptr = mmap(0, size, PROT_READ, MAP_SHARED, fd, map.offset);
    if (ptr == MAP_FAILED) { perror("mmap"); return 1; }

    FILE *f = fopen("/tmp/scanout.raw", "wb");
    fwrite(ptr, 1, size, f);
    fclose(f);
    printf("dumped %zu bytes to /tmp/scanout.raw\n", size);
    return 0;
}

