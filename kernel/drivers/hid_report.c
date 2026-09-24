#include "hid_report.h"

#define ITEM_TYPE_MAIN   0
#define ITEM_TYPE_GLOBAL 1
#define ITEM_TYPE_LOCAL  2

#define MAIN_INPUT          0x08
#define MAIN_COLLECTION     0x0A
#define MAIN_END_COLLECTION 0x0C

#define GLOBAL_USAGE_PAGE  0x00
#define GLOBAL_REPORT_SIZE 0x07
#define GLOBAL_REPORT_ID   0x08
#define GLOBAL_REPORT_COUNT 0x09

#define LOCAL_USAGE     0x00
#define LOCAL_USAGE_MIN 0x01
#define LOCAL_USAGE_MAX 0x02

#define USAGE_PAGE_GENERIC_DESKTOP 0x01
#define USAGE_PAGE_BUTTON          0x09

#define USAGE_MOUSE 0x02
#define USAGE_X     0x30
#define USAGE_Y     0x31
#define USAGE_WHEEL 0x38

#define COLLECTION_APPLICATION 0x01

#define MAX_LOCAL_USAGES 16
#define MAX_REPORT_IDS   16

typedef struct {
    uint8_t id;
    uint16_t bits;
} report_cursor_t;

static uint32_t item_value(const uint8_t *data, uint8_t size) {
    switch (size) {
        case 1: return data[0];
        case 2: return (uint32_t)data[0] | ((uint32_t)data[1] << 8);
        case 4: return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        default: return 0;
    }
}

static uint16_t *cursor_for(report_cursor_t *cursors, int *count, uint8_t id) {
    for (int i = 0; i < *count; i++) {
        if (cursors[i].id == id) {
            return &cursors[i].bits;
        }
    }
    if (*count >= MAX_REPORT_IDS) {
        return &cursors[0].bits;
    }
    cursors[*count].id = id;
    cursors[*count].bits = 0;
    (*count)++;
    return &cursors[*count - 1].bits;
}

int hid_report_find_mouse(const uint8_t *descriptor, uint32_t length, hid_mouse_layout_t *out) {
    if (!descriptor || !out) {
        return 0;
    }

    for (uint32_t i = 0; i < sizeof(*out); i++) {
        ((uint8_t *)out)[i] = 0;
    }

    uint16_t usage_page = 0;
    uint16_t report_size = 0;
    uint16_t report_count = 0;
    uint8_t report_id = 0;
    int descriptor_has_report_ids = 0;

    uint32_t local_usages[MAX_LOCAL_USAGES];
    uint16_t local_usage_pages[MAX_LOCAL_USAGES];
    int local_usage_count = 0;
    uint32_t usage_minimum = 0;
    uint32_t usage_maximum = 0;
    int have_usage_range = 0;

    report_cursor_t cursors[MAX_REPORT_IDS];
    int cursor_count = 0;

    int depth = 0;
    int mouse_depth = -1;
    int found = 0;

    uint32_t offset = 0;
    while (offset < length) {
        uint8_t prefix = descriptor[offset++];
        if (prefix == 0xFE) {
            if (offset + 1 >= length) {
                break;
            }
            uint8_t long_size = descriptor[offset];
            offset += 2u + long_size;
            continue;
        }

        uint8_t size = prefix & 0x03u;
        if (size == 3) {
            size = 4;
        }
        uint8_t type = (prefix >> 2) & 0x03u;
        uint8_t tag = (prefix >> 4) & 0x0Fu;

        if (offset + size > length) {
            break;
        }
        uint32_t value = item_value(descriptor + offset, size);
        offset += size;

        if (type == ITEM_TYPE_GLOBAL) {
            switch (tag) {
                case GLOBAL_USAGE_PAGE: usage_page = (uint16_t)value; break;
                case GLOBAL_REPORT_SIZE: report_size = (uint16_t)value; break;
                case GLOBAL_REPORT_COUNT: report_count = (uint16_t)value; break;
                case GLOBAL_REPORT_ID:
                    report_id = (uint8_t)value;
                    descriptor_has_report_ids = 1;
                    break;
                default: break;
            }
            continue;
        }

        if (type == ITEM_TYPE_LOCAL) {
            switch (tag) {
                case LOCAL_USAGE:
                    if (local_usage_count < MAX_LOCAL_USAGES) {
                        local_usages[local_usage_count] = (size == 4) ? (value & 0xFFFFu) : value;
                        local_usage_pages[local_usage_count] =
                            (size == 4) ? (uint16_t)(value >> 16) : usage_page;
                        local_usage_count++;
                    }
                    break;
                case LOCAL_USAGE_MIN:
                    usage_minimum = value;
                    have_usage_range = 1;
                    break;
                case LOCAL_USAGE_MAX:
                    usage_maximum = value;
                    have_usage_range = 1;
                    break;
                default: break;
            }
            continue;
        }

        if (tag == MAIN_COLLECTION) {
            if (value == COLLECTION_APPLICATION && mouse_depth < 0 && local_usage_count > 0 &&
                local_usage_pages[0] == USAGE_PAGE_GENERIC_DESKTOP &&
                local_usages[0] == USAGE_MOUSE) {
                mouse_depth = depth;
            }
            depth++;
        } else if (tag == MAIN_END_COLLECTION) {
            depth--;
            if (mouse_depth >= 0 && depth <= mouse_depth) {
                mouse_depth = -1;
            }
        } else if (tag == MAIN_INPUT) {
            uint16_t *bits = cursor_for(cursors, &cursor_count, report_id);
            uint16_t start = *bits;
            uint32_t field_bits = (uint32_t)report_size * report_count;
            if (field_bits > 0xFFFFu || (uint32_t)start + field_bits > 0xFFFFu) {
                break;
            }

            int constant = (value & 0x01u) != 0;
            if (mouse_depth >= 0 && !constant && report_size > 0) {
                if (have_usage_range && usage_page == USAGE_PAGE_BUTTON && usage_minimum == 1 &&
                    !out->button_count) {
                    uint32_t buttons = usage_maximum - usage_minimum + 1;
                    if (buttons > report_count) {
                        buttons = report_count;
                    }
                    if (buttons > HID_MOUSE_MAX_BUTTONS) {
                        buttons = HID_MOUSE_MAX_BUTTONS;
                    }
                    out->button_offset = start;
                    out->button_count = (uint8_t)buttons;
                    out->report_id = report_id;
                    found = 1;
                }
                for (int u = 0; u < local_usage_count && (uint16_t)u < report_count; u++) {
                    uint16_t field_offset = (uint16_t)(start + (uint32_t)u * report_size);
                    if (local_usage_pages[u] != USAGE_PAGE_GENERIC_DESKTOP) {
                        continue;
                    }
                    if (local_usages[u] == USAGE_X && !out->has_x) {
                        out->has_x = 1;
                        out->x_offset = field_offset;
                        out->x_bits = (uint8_t)report_size;
                        out->report_id = report_id;
                        found = 1;
                    } else if (local_usages[u] == USAGE_Y && !out->has_y) {
                        out->has_y = 1;
                        out->y_offset = field_offset;
                        out->y_bits = (uint8_t)report_size;
                        out->report_id = report_id;
                        found = 1;
                    } else if (local_usages[u] == USAGE_WHEEL && !out->has_wheel) {
                        out->has_wheel = 1;
                        out->wheel_offset = field_offset;
                        out->wheel_bits = (uint8_t)report_size;
                        out->report_id = report_id;
                        found = 1;
                    }
                }
            }

            *bits = (uint16_t)(start + field_bits);
        }

        local_usage_count = 0;
        have_usage_range = 0;
        usage_minimum = 0;
        usage_maximum = 0;
    }

    if (!found || !out->has_x || !out->has_y) {
        return 0;
    }

    out->has_report_id = (uint8_t)descriptor_has_report_ids;
    for (int i = 0; i < cursor_count; i++) {
        if (cursors[i].id == out->report_id) {
            out->report_bits = cursors[i].bits;
        }
    }
    return 1;
}

static int32_t read_field(const uint8_t *report, uint32_t bytes, uint16_t offset, uint8_t bits,
                          int sign_extend) {
    if (bits == 0 || bits > 32) {
        return 0;
    }
    uint32_t value = 0;
    for (uint8_t i = 0; i < bits; i++) {
        uint32_t bit = (uint32_t)offset + i;
        uint32_t byte = bit / 8u;
        if (byte >= bytes) {
            return 0;
        }
        if (report[byte] & (1u << (bit % 8u))) {
            value |= (1u << i);
        }
    }
    if (sign_extend && bits < 32 && (value & (1u << (bits - 1)))) {
        value |= ~((1u << bits) - 1u);
    }
    return (int32_t)value;
}

int hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *report, uint32_t length,
                     hid_mouse_report_t *out) {
    if (!layout || !report || !out || length == 0) {
        return 0;
    }

    const uint8_t *body = report;
    uint32_t bytes = length;
    if (layout->has_report_id) {
        if (report[0] != layout->report_id) {
            return 0;
        }
        body = report + 1;
        bytes = length - 1;
    }
    if (bytes == 0) {
        return 0;
    }

    out->dx = read_field(body, bytes, layout->x_offset, layout->x_bits, 1);
    out->dy = read_field(body, bytes, layout->y_offset, layout->y_bits, 1);
    out->wheel = layout->has_wheel
                     ? read_field(body, bytes, layout->wheel_offset, layout->wheel_bits, 1)
                     : 0;
    out->buttons = (uint8_t)read_field(body, bytes, layout->button_offset, layout->button_count, 0);
    return 1;
}
