#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "types.h"
#include <SDL.h>

void handle_app_events(AppState *state, Layout *layout, int *running);

#endif