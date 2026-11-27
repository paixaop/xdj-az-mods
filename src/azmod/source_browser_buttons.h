/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_BROWSER_BUTTONS_H
#define AZMOD_SOURCE_BROWSER_BUTTONS_H

#include "azmod-mod.h"

struct source_browser_buttons {
    unsigned down, swallowed;
};

enum {
    SOURCE_BROWSER_NO_VIEW = -1,
    SOURCE_BROWSER_NATIVE_TABS = 0xb4u
};

static int source_browser_key_view(unsigned rising) {
    if (rising & SOURCE_BROWSER_NATIVE_TABS) /* SOURCE, TAG LIST, PLAYLIST, MENU */
        return AZ_SOURCE_VIEW_HIDDEN;
    if (rising & 0x08u)
        return AZ_SOURCE_VIEW_SEARCH;
    if (rising & 0x40u)
        return AZ_SOURCE_VIEW_BROWSE;
    return SOURCE_BROWSER_NO_VIEW;
}

static unsigned source_browser_route_keys(struct source_browser_buttons *state, unsigned buttons,
                                          int selected, int return_to_source, int *view) {
    unsigned rising = buttons & ~state->down;
    state->down = buttons;
    state->swallowed &= buttons;
    *view = SOURCE_BROWSER_NO_VIEW;
    if (selected) {
        unsigned capture = return_to_source ? 0xc8u : 0x48u;
        state->swallowed |= rising & capture;
        *view = source_browser_key_view(rising);
    }
    return buttons & ~state->swallowed;
}

#endif
