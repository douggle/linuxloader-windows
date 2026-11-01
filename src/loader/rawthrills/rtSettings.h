#ifndef RT_SETTINGS_H
#define RT_SETTINGS_H

// The Raw Thrills games' operator settings, as a home setup needs them: a
// webcam instead of the cabinet's upside-down camera, coins instead of the
// dumps' free play. The file the game should read instead of path, or NULL
// to leave the read alone.
const char *rtSettingFile(const char *path);

#endif // RT_SETTINGS_H
