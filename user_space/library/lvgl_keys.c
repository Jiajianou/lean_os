#include "lvgl_keys.h"

#include "input.h"

uint32_t lvgl_translate_key(char character, uint8_t modifiers) {
    switch (character) {
    case KEYBOARD_KEY_UP:
        return LV_KEY_UP;
    case KEYBOARD_KEY_DOWN:
        return LV_KEY_DOWN;
    case KEYBOARD_KEY_LEFT:
        return LV_KEY_LEFT;
    case KEYBOARD_KEY_RIGHT:
        return LV_KEY_RIGHT;
    case '\r':
    case '\n':
        return LV_KEY_ENTER;
    case '\b':
    case 0x7F:
        return LV_KEY_BACKSPACE;
    case 27:
        return LV_KEY_ESC;
    case '\t':
        return (modifiers & KEYBOARD_MOD_SHIFT) ? LV_KEY_PREV : LV_KEY_NEXT;
    default:
        break;
    }
    if ((unsigned char)character < 0x20) {
        return 0;
    }
    return (uint32_t)(unsigned char)character;
}
