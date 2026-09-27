#include "disk_log_area.h"

#include "library/kernel_library.h"

#define MAGIC_LENGTH (sizeof(DISK_LOG_AREA_MAGIC) - 1)

static const uint8_t *find_field(const uint8_t *sector, const char *name) {
    size_t name_length = k_strlen(name);
    for (size_t i = MAGIC_LENGTH; i + name_length < DISK_LOG_AREA_SECTOR_SIZE; i++) {
        if (sector[i - 1] == '\n' && k_memcmp(sector + i, name, name_length) == 0) {
            return sector + i + name_length;
        }
    }
    return 0;
}

static int parse_field(const uint8_t *sector, const char *name, uint32_t *out) {
    const uint8_t *cursor = find_field(sector, name);
    if (!cursor) {
        return -1;
    }
    const uint8_t *end = sector + DISK_LOG_AREA_SECTOR_SIZE;
    uint64_t value = 0;
    int digits = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        value = value * 10u + (uint64_t)(*cursor - '0');
        if (value > 0xFFFFFFFFu) {
            return -1;
        }
        cursor++;
        digits++;
    }
    if (digits == 0 || cursor == end || *cursor != '\n') {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int disk_log_area_parse_header(const uint8_t *sector, uint32_t *next, uint32_t *wrapped,
                               uint32_t *boots) {
    if (k_memcmp(sector, DISK_LOG_AREA_MAGIC, MAGIC_LENGTH) != 0) {
        return -1;
    }
    if (parse_field(sector, "next=", next) != 0 || parse_field(sector, "wrapped=", wrapped) != 0 ||
        parse_field(sector, "boots=", boots) != 0) {
        return -1;
    }
    return 0;
}

static size_t put_field(uint8_t *out, size_t at, const char *name, uint32_t value) {
    size_t name_length = k_strlen(name);
    k_memcpy(out + at, name, name_length);
    at += name_length;
    char digits[10];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count > 0) {
        out[at++] = (uint8_t)digits[--count];
    }
    out[at++] = '\n';
    return at;
}

void disk_log_area_format_header(const disk_log_area_t *area, uint8_t *sector) {
    k_memset(sector, '\n', DISK_LOG_AREA_SECTOR_SIZE);
    k_memcpy(sector, DISK_LOG_AREA_MAGIC, MAGIC_LENGTH);
    size_t at = MAGIC_LENGTH;
    at = put_field(sector, at, "next=", area->next);
    at = put_field(sector, at, "wrapped=", area->wrapped);
    put_field(sector, at, "boots=", area->boots);
}

int disk_log_area_open(disk_log_area_t *area, uint32_t first_lba, uint32_t sectors,
                       disk_log_area_io_t read, disk_log_area_io_t write, void *context) {
    area->first_lba = first_lba;
    area->data_sectors = sectors > 1 ? sectors - 1 : 0;
    if (area->data_sectors > DISK_LOG_AREA_MAX_DATA_SECTORS) {
        area->data_sectors = DISK_LOG_AREA_MAX_DATA_SECTORS;
    }
    area->read = read;
    area->write = write;
    area->context = context;
    area->next = 0;
    area->wrapped = 0;
    area->boots = 0;
    if (area->data_sectors == 0) {
        return DISK_LOG_AREA_NOT_AN_AREA;
    }
    if (read(context, first_lba, 1, area->stage) != 0) {
        return DISK_LOG_AREA_IO_ERROR;
    }
    uint32_t next = 0, wrapped = 0, boots = 0;
    if (disk_log_area_parse_header(area->stage, &next, &wrapped, &boots) != 0) {
        return DISK_LOG_AREA_NOT_AN_AREA;
    }
    uint64_t capacity = (uint64_t)area->data_sectors * DISK_LOG_AREA_SECTOR_SIZE;
    if (next >= capacity) {
        next = 0;
        wrapped = 1;
    }
    area->next = next;
    area->wrapped = wrapped ? 1u : 0u;
    area->boots = boots + 1u;
    k_memset(area->tail, '\n', sizeof(area->tail));
    if (next % DISK_LOG_AREA_SECTOR_SIZE != 0) {
        uint32_t lba = first_lba + 1u + next / DISK_LOG_AREA_SECTOR_SIZE;
        if (read(context, lba, 1, area->tail) != 0) {
            return DISK_LOG_AREA_IO_ERROR;
        }
    }
    return DISK_LOG_AREA_OPENED;
}

int disk_log_area_append(disk_log_area_t *area, const char *data, size_t length) {
    while (length > 0) {
        uint32_t sector = area->next / DISK_LOG_AREA_SECTOR_SIZE;
        uint32_t offset = area->next % DISK_LOG_AREA_SECTOR_SIZE;
        uint32_t room_sectors = area->data_sectors - sector;
        if (room_sectors > DISK_LOG_AREA_STAGE_SECTORS) {
            room_sectors = DISK_LOG_AREA_STAGE_SECTORS;
        }
        size_t capacity = (size_t)room_sectors * DISK_LOG_AREA_SECTOR_SIZE - offset;
        size_t take = length < capacity ? length : capacity;
        uint32_t used_sectors =
            (uint32_t)((offset + take + DISK_LOG_AREA_SECTOR_SIZE - 1) / DISK_LOG_AREA_SECTOR_SIZE);

        k_memset(area->stage, '\n', (size_t)used_sectors * DISK_LOG_AREA_SECTOR_SIZE);
        k_memcpy(area->stage, area->tail, offset);
        k_memcpy(area->stage + offset, data, take);
        if (area->write(area->context, area->first_lba + 1u + sector, used_sectors, area->stage) != 0) {
            return -1;
        }

        uint64_t next = (uint64_t)area->next + take;
        k_memcpy(area->tail, area->stage + (size_t)(used_sectors - 1) * DISK_LOG_AREA_SECTOR_SIZE,
                 DISK_LOG_AREA_SECTOR_SIZE);
        if (next % DISK_LOG_AREA_SECTOR_SIZE == 0) {
            k_memset(area->tail, '\n', sizeof(area->tail));
        }
        if (next >= (uint64_t)area->data_sectors * DISK_LOG_AREA_SECTOR_SIZE) {
            next = 0;
            area->wrapped = 1;
        }
        area->next = (uint32_t)next;
        data += take;
        length -= take;
    }
    return 0;
}

int disk_log_area_write_header(disk_log_area_t *area) {
    disk_log_area_format_header(area, area->stage);
    return area->write(area->context, area->first_lba, 1, area->stage);
}
