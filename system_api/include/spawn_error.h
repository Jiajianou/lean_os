#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define SPAWN_ERR_NOT_FOUND    (-1)
#define SPAWN_ERR_BAD_IMAGE    (-2)
#define SPAWN_ERR_NO_TASK_SLOT (-3)
#define SPAWN_ERR_NO_MEMORY    (-4)

static inline const char *spawn_error_message(long code) {
    switch (code) {
    case SPAWN_ERR_NOT_FOUND:
        return "No such program.";
    case SPAWN_ERR_BAD_IMAGE:
        return "That file is not a program.";
    case SPAWN_ERR_NO_TASK_SLOT:
        return "Too many programs are running.";
    case SPAWN_ERR_NO_MEMORY:
        return "Out of memory.";
    default:
        return "Could not start that program.";
    }
}

#ifdef __cplusplus
}
#endif
