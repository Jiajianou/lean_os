#include "string_utilities.h"
#include "syscall_wrappers.h"
#include "wm.h"

#define WIN_W 200
#define WIN_H 120
#define CONTENT_COLOR 0x00336699u
#define ACCENT_COLOR  0x00CC8822u
#define ACCENT_X      20
#define ACCENT_Y      20
#define ACCENT_SIZE   40

int main(void) {
    int request_file_descriptors[2];
    int response_file_descriptors[2];
    if (sys_pipe_open(WM_REQUEST_PIPE, request_file_descriptors) != 0 || sys_pipe_open(WM_RESPONSE_PIPE, response_file_descriptors) != 0) {
        sys_exit(1);
    }

    wm_create_request_t req;
    memset(&req, 0, sizeof(req));
    req.width = WIN_W;
    req.height = WIN_H;
    req.client_pid = (int32_t)sys_getpid();

    wm_create_response_t response;
    int got_response = 0;
    for (int attempt = 0; attempt < 4 && !got_response; attempt++) {
        if (sys_write(request_file_descriptors[1], &req, sizeof(req)) != (long)sizeof(req)) {
            sys_exit(1);
        }
        long deadline = sys_uptime_ms() + 500;
        while (!got_response && sys_uptime_ms() < deadline) {
            if (sys_pipe_poll(response_file_descriptors[0]) < (long)sizeof(response)) {
                sys_yield();
                continue;
            }
            if (sys_read(response_file_descriptors[0], &response, sizeof(response)) != (long)sizeof(response)) {
                sys_exit(1);
            }
            got_response = (response.client_pid == req.client_pid);
        }
    }
    if (!got_response || response.shm_id < 0) {
        sys_exit(1);
    }

    long vaddr = sys_shared_memory_map(response.shm_id);
    if (vaddr < 0) {
        sys_exit(1);
    }

    uint32_t *pixels = (uint32_t *)vaddr;
    for (int i = 0; i < WIN_W * WIN_H; i++) {
        pixels[i] = CONTENT_COLOR;
    }
    for (int y = ACCENT_Y; y < ACCENT_Y + ACCENT_SIZE; y++) {
        for (int x = ACCENT_X; x < ACCENT_X + ACCENT_SIZE; x++) {
            pixels[y * WIN_W + x] = ACCENT_COLOR;
        }
    }

    return 0;
}
