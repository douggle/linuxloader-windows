#ifndef CONTROL_MAPPER_H
#define CONTROL_MAPPER_H

#ifdef __cplusplus
extern "C"
{
#endif

    // Interactive controls.ini mapping: for every action of a controls
    // section, wait for its control to be pressed and write it into the file.
    // sectionName may be NULL to pick from a menu, and controlsPath is created
    // from the defaults when it does not exist yet. Linux only: it reads the
    // terminal for the "keep/skip/quit" answers and SDL for the controls.
    int runControlMapper(const char *sectionName, const char *controlsPath);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_MAPPER_H */
