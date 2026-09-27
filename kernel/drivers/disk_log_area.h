#pragma once

#include <stddef.h>
#include <stdint.h>

#define DISK_LOG_AREA_SECTOR_SIZE   512u
#define DISK_LOG_AREA_STAGE_SECTORS 64u
#define DISK_LOG_AREA_MAX_DATA_SECTORS 0x7FFFFFu
#define DISK_LOG_AREA_MAGIC         "LEAN_OS LOG AREA 1\n"

typedef int (*disk_log_area_io_t)(void *context, uint32_t lba, uint32_t count, void *buffer);

typedef struct {
    uint32_t first_lba;
    uint32_t data_sectors;
    uint32_t next;
    uint32_t wrapped;
    uint32_t boots;
    disk_log_area_io_t read;
    disk_log_area_io_t write;
    void *context;
    uint8_t tail[DISK_LOG_AREA_SECTOR_SIZE];
    uint8_t stage[DISK_LOG_AREA_STAGE_SECTORS * DISK_LOG_AREA_SECTOR_SIZE];
} disk_log_area_t;

#define DISK_LOG_AREA_OPENED        0
#define DISK_LOG_AREA_IO_ERROR      (-1)
#define DISK_LOG_AREA_NOT_AN_AREA   (-2)

int disk_log_area_open(disk_log_area_t *area, uint32_t first_lba, uint32_t sectors,
                       disk_log_area_io_t read, disk_log_area_io_t write, void *context);

int disk_log_area_append(disk_log_area_t *area, const char *data, size_t length);

int disk_log_area_write_header(disk_log_area_t *area);

void disk_log_area_format_header(const disk_log_area_t *area, uint8_t *sector);

int disk_log_area_parse_header(const uint8_t *sector, uint32_t *next, uint32_t *wrapped,
                               uint32_t *boots);
