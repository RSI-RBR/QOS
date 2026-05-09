#ifndef INPUT_EVENT_H
#define INPUT_EVENT_H

#define QOS_EVENT_NONE              0u
#define QOS_EVENT_KEY_DOWN          1u
#define QOS_EVENT_KEY_UP            2u
#define QOS_EVENT_MOUSE_MOVE        3u
#define QOS_EVENT_MOUSE_BUTTON_DOWN 4u
#define QOS_EVENT_MOUSE_BUTTON_UP   5u
#define QOS_EVENT_MOUSE_WHEEL       6u

#define QOS_EVENT_SOURCE_KEYBOARD 1u
#define QOS_EVENT_SOURCE_MOUSE    2u

#define QOS_KEYMOD_SHIFT 1u
#define QOS_KEYMOD_CTRL  2u
#define QOS_KEYMOD_ALT   4u
#define QOS_KEYMOD_META  8u

#define QOS_MOUSE_LEFT   1u
#define QOS_MOUSE_RIGHT  2u
#define QOS_MOUSE_MIDDLE 4u

typedef struct {
    unsigned int type;
    unsigned int source;
    unsigned int keycode;
    unsigned int ascii;
    unsigned int modifiers;
    unsigned int buttons;
    unsigned int button;
    int x;
    int y;
    int dx;
    int dy;
    int wheel;
    unsigned long tick;
    unsigned long seq;
} qos_event_t;

#endif
