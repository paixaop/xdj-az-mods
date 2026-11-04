/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by community.c after the data/command providers. All registration and
 * delivery state belongs to the UI thread. Producers only publish provider state. */
static struct {
    void (*data)(const az_data_change *, void *);
    void (*command)(const az_command_result *, void *);
    void *data_user, *command_user;
    uint64_t after;
} g_sdk_callbacks[CM_MAX];
static uint64_t g_sdk_command_notified[SCMD_SLOTS];

static int sdk_callback_owner(const char *id, uintptr_t caller, uintptr_t fn, const char *service) {
    cm_mod *c = cm_self(id, caller, service);
    if (!c || g_cm_in_frame || !g_sdk_in_init || c - g_cm != g_cm_loading ||
        !fn || !c->hi || fn < c->lo || fn >= c->hi) return -1;
    return (int) (c - g_cm);
}
static int sdk_on_data_event(const char *id, void (*fn)(const az_data_change *, void *), void *user) {
    int owner = sdk_callback_owner(id, CM_RA(), (uintptr_t) fn, "on_data_event");
    if (owner < 0) return AZ_INVALID;
    if (g_sdk_callbacks[owner].data) return AZ_LIMIT;
    if (pthread_mutex_trylock(&g_st.mu)) return AZ_BUSY;
    uint64_t after = g_st.sequence;
    int exhausted = g_st.events_exhausted;
    pthread_mutex_unlock(&g_st.mu);
    if (exhausted) return AZ_LIMIT;
    g_sdk_callbacks[owner].after = after;
    g_sdk_callbacks[owner].data_user = user;
    g_sdk_callbacks[owner].data = fn;
    return AZ_OK;
}
static int sdk_on_command_complete(const char *id, void (*fn)(const az_command_result *, void *), void *user) {
    int owner = sdk_callback_owner(id, CM_RA(), (uintptr_t) fn, "on_command_complete");
    if (owner < 0) return AZ_INVALID;
    if (g_sdk_callbacks[owner].command) return AZ_LIMIT;
    g_sdk_callbacks[owner].command_user = user;
    g_sdk_callbacks[owner].command = fn;
    return AZ_OK;
}
static void sdk_data_callbacks_tick(void) {
    for (int owner = 0; owner < g_cm_n; ++owner) {
        if (g_cm[owner].state != AZCM_LOADED || st_owner_retired(&g_st, owner)) continue;
        if (g_sdk_callbacks[owner].data) {
            for (int n = 0; n < 16; ++n) {
                az_data_change event = {.size = sizeof event};
                int rc = st_event_read(&g_st, owner, g_sdk_callbacks[owner].after, &event);
                if (rc != AZ_OK) break; /* contention retries on the next UI turn */
                g_sdk_callbacks[owner].after = event.sequence;
                g_sdk_callbacks[owner].data(&event, g_sdk_callbacks[owner].data_user);
                if (g_cm[owner].state != AZCM_LOADED || st_owner_retired(&g_st, owner)) break;
            }
        }
    }
    /* Read retained records instead of a lossy journal: completion is delivered
     * exactly once even if unrelated commands overrun their event ring. Copy one
     * result under the lock and release it before entering mod code. */
    for (int i = 0; i < SCMD_SLOTS; ++i) {
        if (pthread_mutex_trylock(&g_sdk_commands.mu)) break;
        const struct scmd_record *r = &g_sdk_commands.records[i];
        int owner = r->owner;
        int deliver = r->phase == SCMD_FINAL && !r->asynchronous && !r->pinned && !r->awaiting &&
                      r->token != g_sdk_command_notified[i] && owner >= 0 && owner < g_cm_n &&
                      g_cm[owner].state == AZCM_LOADED && !st_owner_retired(&g_st, owner) &&
                      g_sdk_callbacks[owner].command;
        az_command_result result;
        if (deliver) {
            result = r->result;
            g_sdk_command_notified[i] = r->token;
        }
        pthread_mutex_unlock(&g_sdk_commands.mu);
        if (deliver) g_sdk_callbacks[owner].command(&result, g_sdk_callbacks[owner].command_user);
    }
}

/* Autonomous command delivery is drained by the overlay loop, not by
 * community_tick.  At most eight callbacks are reserved per pass; the caller
 * leaves the UI descriptor readable when more remain. */
static unsigned sdk_command_ui_drain(unsigned budget) {
    sdk_command_ui_clear();
    unsigned delivered = 0;
    while (delivered < budget) {
        struct scmd_notification notification;
        int rc = scmd_next_notification(&g_sdk_commands, &notification);
        if (rc != AZ_OK) break;
        if (notification.callback) notification.callback(&notification.completion, notification.user);
        scmd_notification_done(&g_sdk_commands, &notification);
        sdk_command_wake();
        delivered++;
    }
    if (sdk_command_has_pending_notification()) sdk_ui_wake();
    return delivered;
}
