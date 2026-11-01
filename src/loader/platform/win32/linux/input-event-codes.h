#ifndef LL_WIN32_LINUX_INPUT_EVENT_CODES_H
#define LL_WIN32_LINUX_INPUT_EVENT_CODES_H

// <linux/input-event-codes.h> for MinGW.  The evdev controllers' structs
// (input/evdevInput.h) size their tables with these, so the loader keeps the
// Linux numbering; the devices themselves are Linux's (input/evdevInput.c is
// not part of the Windows build).

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW 0x05
#define EV_LED 0x11
#define EV_SND 0x12
#define EV_REP 0x14
#define EV_FF 0x15
#define EV_PWR 0x16
#define EV_FF_STATUS 0x17
#define EV_MAX 0x1f
#define EV_CNT (EV_MAX + 1)

#define KEY_RESERVED 0
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6

#define KEY_MAX 0x2ff
#define KEY_CNT (KEY_MAX + 1)

#define BTN_MISC 0x100
#define BTN_JOYSTICK 0x120
#define BTN_GAMEPAD 0x130
#define BTN_SOUTH 0x130
#define BTN_EAST 0x131
#define BTN_NORTH 0x133
#define BTN_WEST 0x134
#define BTN_TL 0x136
#define BTN_TR 0x137
#define BTN_START 0x13b
#define BTN_SELECT 0x13a

#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02
#define ABS_RX 0x03
#define ABS_RY 0x04
#define ABS_RZ 0x05
#define ABS_THROTTLE 0x06
#define ABS_RUDDER 0x07
#define ABS_WHEEL 0x08
#define ABS_GAS 0x09
#define ABS_BRAKE 0x0a
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#define ABS_MAX 0x3f
#define ABS_CNT (ABS_MAX + 1)

#define REL_X 0x00
#define REL_Y 0x01
#define REL_Z 0x02
#define REL_MAX 0x0f
#define REL_CNT (REL_MAX + 1)

#endif // LL_WIN32_LINUX_INPUT_EVENT_CODES_H
