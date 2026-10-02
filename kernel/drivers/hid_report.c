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

#define MAIN_FEATURE 0x0B

#define GLOBAL_LOGICAL_MINIMUM  0x01
#define GLOBAL_LOGICAL_MAXIMUM  0x02
#define GLOBAL_PHYSICAL_MINIMUM 0x03
#define GLOBAL_PHYSICAL_MAXIMUM 0x04
#define GLOBAL_UNIT_EXPONENT    0x05
#define GLOBAL_UNIT             0x06
#define GLOBAL_PUSH             0x0A
#define GLOBAL_POP              0x0B

#define USAGE_PAGE_DIGITIZER 0x0D

#define USAGE_TOUCH_PAD             0x05
#define USAGE_DEVICE_CONFIGURATION  0x0E
#define USAGE_FINGER                0x22
#define USAGE_TIP_SWITCH            0x42
#define USAGE_CONFIDENCE            0x47
#define USAGE_CONTACT_IDENTIFIER    0x51
#define USAGE_INPUT_MODE            0x52
#define USAGE_CONTACT_COUNT         0x54
#define USAGE_SCAN_TIME             0x56
#define USAGE_SURFACE_SWITCH        0x57
#define USAGE_BUTTON_SWITCH         0x58

#define COLLECTION_LOGICAL 0x02

#define UNIT_CENTIMETRE 0x11
#define UNIT_INCH       0x13

#define MAX_GLOBAL_STACK 4
#define MAX_COLLECTION_DEPTH 16

typedef struct {
    uint16_t usage_page;
    uint16_t report_size;
    uint16_t report_count;
    uint8_t report_id;
    int32_t logical_minimum;
    int32_t logical_maximum;
    int32_t physical_minimum;
    int32_t physical_maximum;
    int32_t unit_exponent;
    uint32_t unit;
} global_state_t;

typedef struct {
    uint8_t id;
    uint8_t feature;
    uint16_t bits;
} typed_cursor_t;

static int32_t signed_item_value(uint32_t value, uint8_t size) {
    if (size == 1 && (value & 0x80u)) {
        return (int32_t)(value | 0xFFFFFF00u);
    }
    if (size == 2 && (value & 0x8000u)) {
        return (int32_t)(value | 0xFFFF0000u);
    }
    return (int32_t)value;
}

static uint16_t *typed_cursor_for(typed_cursor_t *cursors, int *count, uint8_t id, uint8_t feature) {
    for (int i = 0; i < *count; i++) {
        if (cursors[i].id == id && cursors[i].feature == feature) {
            return &cursors[i].bits;
        }
    }
    if (*count >= MAX_REPORT_IDS * 2) {
        return &cursors[0].bits;
    }
    cursors[*count].id = id;
    cursors[*count].feature = feature;
    cursors[*count].bits = 0;
    (*count)++;
    return &cursors[*count - 1].bits;
}

static uint16_t typed_cursor_bits(const typed_cursor_t *cursors, int count, uint8_t id, uint8_t feature) {
    for (int i = 0; i < count; i++) {
        if (cursors[i].id == id && cursors[i].feature == feature) {
            return cursors[i].bits;
        }
    }
    return 0;
}

static void set_field(hid_field_t *field, uint16_t offset, uint16_t bits) {
    if (field->present || bits == 0 || bits > 32) {
        return;
    }
    field->present = 1;
    field->offset = offset;
    field->bits = (uint8_t)bits;
}

/* What one millimetre-tenth count of the physical extent is, from the unit
   and its exponent. The Precision Touchpad specification asks for
   centimetres or inches with an exponent, and a device that says neither has
   its size left at zero for the caller to guess. */
static uint32_t physical_tenths_mm(int32_t extent, uint32_t unit, int32_t exponent) {
    if (extent <= 0) {
        return 0;
    }
    uint64_t value = (uint64_t)extent;
    int32_t power;
    if ((unit & 0xFFu) == UNIT_CENTIMETRE) {
        power = exponent + 2;
    } else if ((unit & 0xFFu) == UNIT_INCH) {
        value *= 254u;
        power = exponent;
    } else {
        return 0;
    }
    for (; power > 0; power--) {
        value *= 10u;
    }
    for (; power < 0; power++) {
        value /= 10u;
    }
    return value > 0xFFFFFFu ? 0 : (uint32_t)value;
}

int hid_report_find_touchpad(const uint8_t *descriptor, uint32_t length, hid_touchpad_layout_t *out) {
    if (!descriptor || !out) {
        return 0;
    }
    for (uint32_t i = 0; i < sizeof(*out); i++) {
        ((uint8_t *)out)[i] = 0;
    }

    global_state_t global = {0};
    global_state_t stack[MAX_GLOBAL_STACK];
    int stack_depth = 0;

    uint32_t local_usages[MAX_LOCAL_USAGES];
    uint16_t local_usage_pages[MAX_LOCAL_USAGES];
    int local_usage_count = 0;
    uint32_t usage_minimum = 0;
    uint32_t usage_maximum = 0;
    int have_usage_range = 0;

    typed_cursor_t cursors[MAX_REPORT_IDS * 2];
    int cursor_count = 0;

    uint8_t collection_kind[MAX_COLLECTION_DEPTH];
    int depth = 0;
    int touchpad_depth = -1;
    int finger = -1;
    int have_touchpad_report = 0;
    int have_x_extent = 0;
    int have_y_extent = 0;

    enum { KIND_OTHER = 0, KIND_TOUCHPAD, KIND_FINGER };

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
                case GLOBAL_USAGE_PAGE: global.usage_page = (uint16_t)value; break;
                case GLOBAL_REPORT_SIZE: global.report_size = (uint16_t)value; break;
                case GLOBAL_REPORT_COUNT: global.report_count = (uint16_t)value; break;
                case GLOBAL_REPORT_ID: global.report_id = (uint8_t)value; break;
                case GLOBAL_LOGICAL_MINIMUM: global.logical_minimum = signed_item_value(value, size); break;
                case GLOBAL_LOGICAL_MAXIMUM:
                    global.logical_maximum = signed_item_value(value, size);
                    if (global.logical_maximum < global.logical_minimum) {
                        global.logical_maximum = (int32_t)value;
                    }
                    break;
                case GLOBAL_PHYSICAL_MINIMUM: global.physical_minimum = signed_item_value(value, size); break;
                case GLOBAL_PHYSICAL_MAXIMUM:
                    global.physical_maximum = signed_item_value(value, size);
                    if (global.physical_maximum < global.physical_minimum) {
                        global.physical_maximum = (int32_t)value;
                    }
                    break;
                case GLOBAL_UNIT: global.unit = value; break;
                case GLOBAL_UNIT_EXPONENT:
                    global.unit_exponent = (value & 0x08u) && value <= 0x0Fu ? (int32_t)value - 16
                                                                              : signed_item_value(value, size);
                    break;
                case GLOBAL_PUSH:
                    if (stack_depth < MAX_GLOBAL_STACK) {
                        stack[stack_depth++] = global;
                    }
                    break;
                case GLOBAL_POP:
                    if (stack_depth > 0) {
                        global = stack[--stack_depth];
                    }
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
                            (size == 4) ? (uint16_t)(value >> 16) : global.usage_page;
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
            uint8_t kind = KIND_OTHER;
            int is_digitizer = local_usage_count > 0 && local_usage_pages[0] == USAGE_PAGE_DIGITIZER;
            if (value == COLLECTION_APPLICATION && is_digitizer && local_usages[0] == USAGE_TOUCH_PAD &&
                touchpad_depth < 0 && !have_touchpad_report) {
                kind = KIND_TOUCHPAD;
                touchpad_depth = depth;
            } else if (value == COLLECTION_LOGICAL && is_digitizer && local_usages[0] == USAGE_FINGER &&
                       touchpad_depth >= 0 && finger < 0 && out->finger_count < HID_TOUCHPAD_MAX_CONTACTS) {
                kind = KIND_FINGER;
                finger = out->finger_count++;
            }
            if (depth < MAX_COLLECTION_DEPTH) {
                collection_kind[depth] = kind;
            }
            depth++;
        } else if (tag == MAIN_END_COLLECTION) {
            if (depth > 0) {
                depth--;
                uint8_t kind = depth < MAX_COLLECTION_DEPTH ? collection_kind[depth] : KIND_OTHER;
                if (kind == KIND_FINGER) {
                    finger = -1;
                } else if (kind == KIND_TOUCHPAD) {
                    touchpad_depth = -1;
                    if (out->finger_count > 0) {
                        have_touchpad_report = 1;
                    }
                }
            }
        } else if (tag == MAIN_INPUT || tag == MAIN_FEATURE) {
            uint8_t feature = tag == MAIN_FEATURE;
            uint16_t *bits = typed_cursor_for(cursors, &cursor_count, global.report_id, feature);
            uint16_t start = *bits;
            uint32_t field_bits = (uint32_t)global.report_size * global.report_count;
            if (field_bits > 0xFFFFu || (uint32_t)start + field_bits > 0xFFFFu) {
                break;
            }
            int constant = (value & 0x01u) != 0;
            for (uint32_t u = 0; !constant && u < global.report_count && global.report_size > 0; u++) {
                uint16_t page;
                uint32_t usage;
                if (have_usage_range) {
                    usage = usage_minimum + u;
                    if (usage > usage_maximum) {
                        break;
                    }
                    page = global.usage_page;
                } else if (local_usage_count > 0) {
                    int k = (int)u < local_usage_count ? (int)u : local_usage_count - 1;
                    usage = local_usages[k];
                    page = local_usage_pages[k];
                } else {
                    break;
                }
                uint16_t at = (uint16_t)(start + u * global.report_size);
                uint16_t width = global.report_size;

                if (feature) {
                    if (page != USAGE_PAGE_DIGITIZER) {
                        continue;
                    }
                    if (usage == USAGE_INPUT_MODE && !out->input_mode.present) {
                        set_field(&out->input_mode, at, width);
                        out->input_mode_report_id = global.report_id;
                    } else if ((usage == USAGE_SURFACE_SWITCH || usage == USAGE_BUTTON_SWITCH)) {
                        hid_field_t *which = usage == USAGE_SURFACE_SWITCH ? &out->surface_switch
                                                                             : &out->button_switch;
                        if (!which->present && (!out->surface_switch.present && !out->button_switch.present
                                                ? 1 : out->switches_report_id == global.report_id)) {
                            set_field(which, at, width);
                            out->switches_report_id = global.report_id;
                        }
                    }
                    continue;
                }

                if (touchpad_depth < 0) {
                    continue;
                }
                if (out->finger_count > 0 && global.report_id != out->report_id && out->fingers[0].tip.present) {
                    continue;
                }
                if (finger >= 0) {
                    hid_touchpad_finger_layout_t *f = &out->fingers[finger];
                    if (page == USAGE_PAGE_DIGITIZER && usage == USAGE_TIP_SWITCH) {
                        set_field(&f->tip, at, width);
                        out->report_id = global.report_id;
                    } else if (page == USAGE_PAGE_DIGITIZER && usage == USAGE_CONFIDENCE) {
                        set_field(&f->confidence, at, width);
                    } else if (page == USAGE_PAGE_DIGITIZER && usage == USAGE_CONTACT_IDENTIFIER) {
                        set_field(&f->contact_id, at, width);
                    } else if (page == USAGE_PAGE_GENERIC_DESKTOP && (usage == USAGE_X || usage == USAGE_Y)) {
                        int is_x = usage == USAGE_X;
                        set_field(is_x ? &f->x : &f->y, at, width);
                        int *have = is_x ? &have_x_extent : &have_y_extent;
                        if (!*have) {
                            *have = 1;
                            uint32_t tenths = physical_tenths_mm(global.physical_maximum - global.physical_minimum,
                                                                 global.unit, global.unit_exponent);
                            if (is_x) {
                                out->x_minimum = global.logical_minimum;
                                out->x_maximum = global.logical_maximum;
                                out->width_tenths_mm = tenths;
                            } else {
                                out->y_minimum = global.logical_minimum;
                                out->y_maximum = global.logical_maximum;
                                out->height_tenths_mm = tenths;
                            }
                        }
                    }
                } else if (page == USAGE_PAGE_DIGITIZER && usage == USAGE_CONTACT_COUNT) {
                    set_field(&out->contact_count, at, width);
                } else if (page == USAGE_PAGE_DIGITIZER && usage == USAGE_SCAN_TIME) {
                    set_field(&out->scan_time, at, width);
                } else if (page == USAGE_PAGE_BUTTON && usage == 1) {
                    set_field(&out->button, at, width);
                }
            }
            *bits = (uint16_t)(start + field_bits);
        }

        local_usage_count = 0;
        have_usage_range = 0;
        usage_minimum = 0;
        usage_maximum = 0;
    }

    if (out->finger_count == 0) {
        return 0;
    }
    const hid_touchpad_finger_layout_t *first = &out->fingers[0];
    if (!first->tip.present || !first->x.present || !first->y.present || out->x_maximum <= out->x_minimum ||
        out->y_maximum <= out->y_minimum) {
        return 0;
    }
    out->report_bits = typed_cursor_bits(cursors, cursor_count, out->report_id, 0);
    if (out->input_mode.present) {
        out->input_mode_report_bits = typed_cursor_bits(cursors, cursor_count, out->input_mode_report_id, 1);
    }
    if (out->surface_switch.present || out->button_switch.present) {
        out->switches_report_bits = typed_cursor_bits(cursors, cursor_count, out->switches_report_id, 1);
    }
    return 1;
}

static int32_t read_layout_field(const uint8_t *body, uint32_t bytes, const hid_field_t *field, int sign_extend) {
    return field->present ? read_field(body, bytes, field->offset, field->bits, sign_extend) : 0;
}

int hid_touchpad_decode(const hid_touchpad_layout_t *layout, const uint8_t *report, uint32_t length,
                        hid_touchpad_report_t *out) {
    if (!layout || !report || !out || length == 0) {
        return 0;
    }
    const uint8_t *body = report;
    uint32_t bytes = length;
    if (layout->report_id != 0) {
        if (report[0] != layout->report_id) {
            return 0;
        }
        body++;
        bytes--;
    }
    if (bytes * 8u < layout->report_bits) {
        return 0;
    }
    for (uint32_t i = 0; i < sizeof(*out); i++) {
        ((uint8_t *)out)[i] = 0;
    }
    out->slots = layout->finger_count;
    for (uint8_t slot = 0; slot < layout->finger_count; slot++) {
        const hid_touchpad_finger_layout_t *f = &layout->fingers[slot];
        hid_touchpad_contact_t *c = &out->contacts[slot];
        c->touching = read_layout_field(body, bytes, &f->tip, 0) != 0;
        c->confident = f->confidence.present ? read_layout_field(body, bytes, &f->confidence, 0) != 0 : 1;
        c->id = f->contact_id.present ? (uint8_t)read_layout_field(body, bytes, &f->contact_id, 0) : slot;
        c->x = read_layout_field(body, bytes, &f->x, layout->x_minimum < 0);
        c->y = read_layout_field(body, bytes, &f->y, layout->y_minimum < 0);
    }
    out->has_contact_count = layout->contact_count.present;
    out->contact_count = (uint8_t)read_layout_field(body, bytes, &layout->contact_count, 0);
    out->button = read_layout_field(body, bytes, &layout->button, 0) != 0;
    return 1;
}

static void write_field(uint8_t *out, uint32_t capacity, const hid_field_t *field, uint32_t value) {
    for (uint8_t i = 0; field->present && i < field->bits; i++) {
        uint32_t bit = (uint32_t)field->offset + i;
        if (bit / 8u >= capacity) {
            return;
        }
        if (value & (1u << i)) {
            out[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
        } else {
            out[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
        }
    }
}

static uint32_t clear_report(uint8_t *out, uint32_t capacity, uint16_t report_bits) {
    uint32_t bytes = ((uint32_t)report_bits + 7u) / 8u;
    if (bytes == 0 || bytes > capacity) {
        return 0;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        out[i] = 0;
    }
    return bytes;
}

uint32_t hid_touchpad_build_input_mode(const hid_touchpad_layout_t *layout, uint8_t mode, uint8_t *out,
                                       uint32_t capacity) {
    if (!layout || !out || !layout->input_mode.present) {
        return 0;
    }
    uint32_t bytes = clear_report(out, capacity, layout->input_mode_report_bits);
    if (bytes == 0) {
        return 0;
    }
    write_field(out, bytes, &layout->input_mode, mode);
    if (layout->switches_report_id == layout->input_mode_report_id) {
        write_field(out, bytes, &layout->surface_switch, 1);
        write_field(out, bytes, &layout->button_switch, 1);
    }
    return bytes;
}

uint32_t hid_touchpad_build_switches(const hid_touchpad_layout_t *layout, uint8_t *out, uint32_t capacity) {
    if (!layout || !out || (!layout->surface_switch.present && !layout->button_switch.present)) {
        return 0;
    }
    if (layout->input_mode.present && layout->switches_report_id == layout->input_mode_report_id) {
        return 0;
    }
    uint32_t bytes = clear_report(out, capacity, layout->switches_report_bits);
    if (bytes == 0) {
        return 0;
    }
    write_field(out, bytes, &layout->surface_switch, 1);
    write_field(out, bytes, &layout->button_switch, 1);
    return bytes;
}
