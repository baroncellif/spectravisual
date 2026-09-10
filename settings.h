#ifndef SETTINGS_H
#define SETTINGS_H

#include "types.h"

/* Display settings and their own window.
 *
 * The file lives next to the executable, not in the working directory: the
 * program is started from whichever folder holds the data of the day, and the
 * way the user likes to see a spectrum has nothing to do with that folder. */
void settings_init(AppState *state, const char *argv0);   /* defaults, then load */
int  settings_save(AppState *state);
void settings_restore_defaults(AppState *state);
const char *settings_file_path(void);

/* Applies the saved starting values to the tools of a fresh session. */
void settings_apply_defaults(AppState *state);

/* Builds the path of a working file, under the configured data folder when one
   is set and next to the launch directory otherwise. */
void settings_data_file(const AppState *state, const char *name, char *out, size_t size);

/* Colour of the plot ground for the selected background preset. */
SDL_Color settings_plot_bg(const AppState *state);

void settings_open(AppState *state);
void settings_close(AppState *state);
void settings_dispose(AppState *state);
int  settings_handle_event(AppState *state, const SDL_Event *event);
void settings_render(AppState *state);

#endif
