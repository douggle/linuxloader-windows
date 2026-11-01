// Interactive controls.ini mapping.
//
// controls.ini maps a logical action (P1_Steer, P1_Gas, ...) to the physical
// inputs that drive it ("GC0_AXIS_LEFTX", "JOY0_BUTTON_3", ...). Writing those
// strings by hand means running --list-controllers and copying numbers across,
// so this walks a section's actions instead: it waits for the control to be
// pressed, says what it read, and writes it into the file.
//
// The inputs are captured through SDL, exactly the way the loader reads them,
// so what is written here always parses back. Keyboard bindings are left
// alone: the answers to the prompts come from the keyboard.

#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "controlMapper.h"
#include "controlIniGen.h"
#include "sdlInput.h"
#include "../config/iniParser.h"
#include "../mainShared.h"

// The default binding tables of controlIniGen.c: they list every action of a
// section, in the order the generated file lists them.
extern const ControlBinding gDefaultCommonBindings[];
extern const size_t gDefaultCommonBindingsSize;
extern const ControlBinding gDefaultDigitalBindings[];
extern const size_t gDefaultDigitalBindingsSize;
extern const ControlBinding gDefaultDrivingBindings[];
extern const size_t gDefaultDrivingBindingsSize;
extern const ControlBinding gDefaultFlyingBindings[];
extern const size_t gDefaultFlyingBindingsSize;
extern const ControlBinding gDefaultShootingBindings[];
extern const size_t gDefaultShootingBindingsSize;
extern const ControlBinding gDefaultMahjongBindings[];
extern const size_t gDefaultMahjongBindingsSize;

#define MAPPER_SECTIONS 6
#define MAPPER_MAX_ACTIONS 64
#define MAPPER_MAX_DEVICES 8
// An axis has to move this far from where it rested for the control to count:
// sticks sit at 0 and triggers at the bottom of their range, and they jitter.
#define MAPPER_AXIS_TRAVEL 12000
// An axis is back at rest once it is this close to where it rested.
#define MAPPER_AXIS_REST (MAPPER_AXIS_TRAVEL / 3)

typedef struct
{
    const char *name;
    const ControlBinding *bindings;
    size_t count;
} MapSection;

typedef struct
{
    SDL_JoystickID id;
    bool gamepad;
    Sint16 restGamepad[SDL_GAMEPAD_AXIS_COUNT];
    Sint16 restJoystick[MAX_JOY_AXES];
} MapDevice;

typedef enum
{
    MAPPER_KEEP,
    MAPPER_SKIP,
    MAPPER_QUIT,
    MAPPER_CAPTURED
} MapAnswer;

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

static bool sectionAt(int index, MapSection *out)
{
    const MapSection table[MAPPER_SECTIONS] = {
        {"Common", gDefaultCommonBindings, gDefaultCommonBindingsSize},
        {"Digital", gDefaultDigitalBindings, gDefaultDigitalBindingsSize},
        {"Driving", gDefaultDrivingBindings, gDefaultDrivingBindingsSize},
        {"Flying", gDefaultFlyingBindings, gDefaultFlyingBindingsSize},
        {"Shooting", gDefaultShootingBindings, gDefaultShootingBindingsSize},
        {"Mahjong", gDefaultMahjongBindings, gDefaultMahjongBindingsSize},
    };

    if (index < 0 || index >= MAPPER_SECTIONS)
        return false;
    *out = table[index];
    return true;
}

static int chooseSection(const char *wanted)
{
    MapSection section;

    if (wanted)
        for (int i = 0; sectionAt(i, &section); i++)
            if (!strcasecmp(section.name, wanted))
                return i;

    printf("Which controls do you want to map?\n");
    for (int i = 0; sectionAt(i, &section); i++)
        printf("  %d. %s\n", i + 1, section.name);
    printf("Section [1-%d]: ", MAPPER_SECTIONS);
    fflush(stdout);

    char line[32];
    if (!fgets(line, sizeof(line), stdin))
        return -1;
    int picked = atoi(line);
    return picked >= 1 && picked <= MAPPER_SECTIONS ? picked - 1 : -1;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

static int openDevices(MapDevice *devices)
{
    int count = 0, found = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&found);

    if (!ids)
        return 0;

    for (int i = 0; i < found && count < MAPPER_MAX_DEVICES; i++)
    {
        bool gamepad = false;
        SDL_Gamepad *pad;
        SDL_Joystick *stick;

        // A gamepad is opened as one, so its buttons and axes arrive with the
        // names controls.ini uses; anything else is read as a joystick.
        if (SDL_IsGamepad(ids[i]) && (pad = SDL_OpenGamepad(ids[i])))
        {
            gamepad = true;
            printf("  %d: %s (gamepad)\n", count, SDL_GetGamepadName(pad));
        }
        else if ((stick = SDL_OpenJoystick(ids[i])))
            printf("  %d: %s (joystick)\n", count, SDL_GetJoystickName(stick));
        else
            continue;

        devices[count].id = ids[i];
        devices[count].gamepad = gamepad;
        count++;
    }
    SDL_free(ids);
    return count;
}

static int deviceIndex(const MapDevice *devices, int count, SDL_JoystickID id)
{
    for (int i = 0; i < count; i++)
        if (devices[i].id == id)
            return i;
    return -1;
}

// Where every axis rests, read once before the first prompt while nothing is
// being touched. It stays the reference for the whole pass: a stick pushed
// back to rest reports its travel back, which must not count as a press.
static void readRest(MapDevice *devices, int count)
{
    SDL_Event event;

    // A device's axes are only known once SDL has seen its first events:
    // let them arrive, then read where everything rests.
    SDL_PumpEvents();
    SDL_Delay(150);
    SDL_PumpEvents();
    for (int i = 0; i < count; i++)
    {
        if (devices[i].gamepad)
        {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(devices[i].id);
            for (int axis = 0; pad && axis < SDL_GAMEPAD_AXIS_COUNT; axis++)
                devices[i].restGamepad[axis] = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)axis);
        }
        else
        {
            SDL_Joystick *stick = SDL_GetJoystickFromID(devices[i].id);
            for (int axis = 0; stick && axis < MAX_JOY_AXES; axis++)
                devices[i].restJoystick[axis] = SDL_GetJoystickAxis(stick, axis);
        }
    }
    // Only what is pressed from here on counts, not the arrival of the
    // devices' current state.
    while (SDL_PollEvent(&event))
        ;
}

// Wait for every axis to come back to where it rested, dropping the events of
// that return: releasing the control just captured is not the next action.
static void waitForRest(const MapDevice *devices, int count)
{
    SDL_Event event;

    for (int waited = 0; waited < 2000; waited += 20)
    {
        bool atRest = true;

        while (SDL_PollEvent(&event))
            ;
        for (int i = 0; i < count && atRest; i++)
        {
            if (devices[i].gamepad)
            {
                SDL_Gamepad *pad = SDL_GetGamepadFromID(devices[i].id);
                for (int axis = 0; pad && axis < SDL_GAMEPAD_AXIS_COUNT; axis++)
                    if (abs(SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)axis) - devices[i].restGamepad[axis]) > MAPPER_AXIS_REST)
                        atRest = false;
            }
            else
            {
                SDL_Joystick *stick = SDL_GetJoystickFromID(devices[i].id);
                for (int axis = 0; stick && axis < MAX_JOY_AXES; axis++)
                    if (abs(SDL_GetJoystickAxis(stick, axis) - devices[i].restJoystick[axis]) > MAPPER_AXIS_REST)
                        atRest = false;
            }
        }
        if (atRest)
            return;
        SDL_Delay(20);
    }
    // Still held after two seconds: let the next prompt read it as it is.
    while (SDL_PollEvent(&event))
        ;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

static void upperCopy(const char *src, char *dst, size_t size)
{
    size_t i;

    for (i = 0; src[i] && i + 1 < size; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

static const char *axisDirection(int travel)
{
    if (travel > MAPPER_AXIS_TRAVEL)
        return "_POSITIVE";
    if (travel < -MAPPER_AXIS_TRAVEL)
        return "_NEGATIVE";
    return NULL;
}

// Turn one SDL event into the controls.ini string for it, if it is a control
// being pressed or moved far enough to count.
static bool captureEvent(const MapDevice *devices, int count, const SDL_Event *event, char *out, size_t size)
{
    int index;
    char name[64];

    switch (event->type)
    {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        {
            const char *button;

            if ((index = deviceIndex(devices, count, event->gbutton.which)) < 0)
                return false;
            if (!(button = SDL_GetGamepadStringForButton(event->gbutton.button)))
                return false;
            upperCopy(button, name, sizeof(name));
            snprintf(out, size, "GC%d_BUTTON_%s", index, name);
            return true;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        {
            const char *axis, *direction;
            int travel;

            if ((index = deviceIndex(devices, count, event->gaxis.which)) < 0)
                return false;
            travel = event->gaxis.value - devices[index].restGamepad[event->gaxis.axis];
            if (!(direction = axisDirection(travel)))
                return false;
            if (!(axis = SDL_GetGamepadStringForAxis(event->gaxis.axis)))
                return false;
            upperCopy(axis, name, sizeof(name));
            snprintf(out, size, "GC%d_AXIS_%s%s", index, name, direction);
            return true;
        }

        // A gamepad also reports its buttons and axes as a joystick: those
        // have been handled above, by their gamepad names.
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            if ((index = deviceIndex(devices, count, event->jbutton.which)) < 0 || devices[index].gamepad)
                return false;
            snprintf(out, size, "JOY%d_BUTTON_%d", index, event->jbutton.button);
            return true;

        case SDL_EVENT_JOYSTICK_HAT_MOTION:
        {
            const char *direction = NULL;

            if ((index = deviceIndex(devices, count, event->jhat.which)) < 0 || devices[index].gamepad)
                return false;
            if (event->jhat.value & SDL_HAT_UP)
                direction = "UP";
            else if (event->jhat.value & SDL_HAT_DOWN)
                direction = "DOWN";
            else if (event->jhat.value & SDL_HAT_LEFT)
                direction = "LEFT";
            else if (event->jhat.value & SDL_HAT_RIGHT)
                direction = "RIGHT";
            if (!direction)
                return false;
            snprintf(out, size, "JOY%d_HAT%d_%s", index, event->jhat.hat, direction);
            return true;
        }

        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        {
            const char *direction;
            int travel;

            if ((index = deviceIndex(devices, count, event->jaxis.which)) < 0 || devices[index].gamepad)
                return false;
            travel = event->jaxis.value - devices[index].restJoystick[event->jaxis.axis];
            if (!(direction = axisDirection(travel)))
                return false;
            snprintf(out, size, "JOY%d_AXIS_%d%s", index, event->jaxis.axis, direction);
            return true;
        }

        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// The terminal's answers
// ---------------------------------------------------------------------------

// A whole line typed on the terminal, without blocking: 'n' for Enter (keep),
// 's' to skip, 'q' to finish. -1 when nothing was typed.
static int readAnswer(void)
{
    struct pollfd watch = {STDIN_FILENO, POLLIN, 0};
    char typed;
    int answer = -1;

    if (poll(&watch, 1, 0) <= 0)
        return -1;
    // End of input (Ctrl-D, or a script that ran out of answers) finishes.
    if (read(STDIN_FILENO, &typed, 1) != 1)
        return 'q';
    if (typed == '\n' || typed == '\r')
        return '\n';

    answer = tolower((unsigned char)typed);
    // Everything else typed on that line is ignored.
    while (poll(&watch, 1, 0) > 0 && read(STDIN_FILENO, &typed, 1) == 1 && typed != '\n')
        ;
    return answer;
}

static MapAnswer waitForControl(MapDevice *devices, int count, char *out, size_t size)
{
    for (;;)
    {
        SDL_Event event;
        int answer;

        while (SDL_PollEvent(&event))
        {
            if (captureEvent(devices, count, &event, out, size))
            {
                // Let the control come back to rest before the next prompt.
                SDL_Delay(200);
                while (SDL_PollEvent(&event))
                    ;
                return MAPPER_CAPTURED;
            }
        }

        answer = readAnswer();
        if (answer == '\n')
            return MAPPER_KEEP;
        if (answer == 's')
            return MAPPER_SKIP;
        if (answer == 'q')
            return MAPPER_QUIT;

        SDL_Delay(10);
    }
}

// ---------------------------------------------------------------------------
// The walk through a section
// ---------------------------------------------------------------------------

static bool alreadyAsked(char keys[][128], int count, const char *key)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(keys[i], key))
            return true;
    return false;
}

int runControlMapper(const char *sectionName, const char *controlsPath)
{
    MapSection section;
    MapDevice devices[MAPPER_MAX_DEVICES];
    char asked[MAPPER_MAX_ACTIONS][128];
    int askedCount = 0, mapped = 0, deviceCount, chosen;
    IniConfig *ini;

    if ((chosen = chooseSection(sectionName)) < 0 || !sectionAt(chosen, &section))
        return PARSE_ARGS_HELP;

    // Start from the generated file: a mapping pass edits a complete,
    // working controls.ini rather than inventing one. (The ini helpers
    // return 1 for success.)
    if (access(controlsPath, R_OK) != 0 && createDefaultControlsIni(controlsPath) <= 0)
        return PARSE_ARGS_HELP;
    if (!(ini = iniLoad(controlsPath)))
    {
        printf("Could not read %s\n", controlsPath);
        return PARSE_ARGS_HELP;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD))
    {
        printf("Could not initialize SDL: %s\n", SDL_GetError());
        iniFree(ini);
        return PARSE_ARGS_HELP;
    }

    printf("\nPress a button or move an axis when asked.\n");
    printf("Enter keeps the current mapping, s skips the action, q saves and stops.\n\n");
    printf("Devices:\n");
    if ((deviceCount = openDevices(devices)) == 0)
    {
        printf("  none found: connect the gamepad, joystick or wheel and run this again.\n");
        SDL_Quit();
        iniFree(ini);
        return PARSE_ARGS_HELP;
    }
    printf("\n");

    readRest(devices, deviceCount);

    for (size_t i = 0; i < section.count && askedCount < MAPPER_MAX_ACTIONS; i++)
    {
        const ControlBinding *binding = &section.bindings[i];
        const char *current;
        char key[128], captured[128];

        getLogicalActionString(binding, key, sizeof(key), section.name);
        if (alreadyAsked(asked, askedCount, key))
            continue;
        snprintf(asked[askedCount++], sizeof(asked[0]), "%s", key);

        // The control just captured is still on its way back to rest: wait
        // for it, so the prompt is up only when a press will register.
        waitForRest(devices, deviceCount);

        current = iniGetValue(ini, section.name, key);
        printf("[%d/%d] %-22s %s\n", askedCount, (int)section.count, key,
               current && current[0] ? current : "(not mapped)");
        printf("       > ");
        fflush(stdout);

        switch (waitForControl(devices, deviceCount, captured, sizeof(captured)))
        {
            case MAPPER_CAPTURED:
                printf("%s\n", captured);
                iniSetValue(ini, section.name, key, captured);
                mapped++;
                break;
            case MAPPER_SKIP:
                printf("skipped\n");
                break;
            case MAPPER_QUIT:
                i = section.count;
                break;
            case MAPPER_KEEP:
            default:
                printf("kept\n");
                break;
        }
    }

    if (iniSave(ini, controlsPath) <= 0)
        printf("\nCould not write %s\n", controlsPath);
    else
        printf("\n%d mapping(s) written to %s (%s section).\n", mapped, controlsPath, section.name);

    iniFree(ini);
    SDL_Quit();
    return PARSE_ARGS_HELP;
}
