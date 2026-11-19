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
static int sdk_data_owner_active(int owner) {
    return g_cm[owner].state == AZCM_LOADED && !st_owner_retired(&g_st, owner);
}
static void sdk_data_owner_callbacks(int owner) {
    if (!sdk_data_owner_active(owner) || !g_sdk_callbacks[owner].data) return;
    for (int n = 0; n < 16; ++n) {
        az_data_change event = {.size = sizeof event};
        int rc = st_event_read(&g_st, owner, g_sdk_callbacks[owner].after, &event);
        if (rc != AZ_OK) break; /* Contention retries on the next UI turn. */
        g_sdk_callbacks[owner].after = event.sequence;
        g_sdk_callbacks[owner].data(&event, g_sdk_callbacks[owner].data_user);
        if (!sdk_data_owner_active(owner)) break;
    }
}
static int sdk_command_callback_ready(const struct scmd_record *record, int slot) {
    return record->phase == SCMD_FINAL && !record->retired && !record->asynchronous && !record->pinned &&
           !record->awaiting && record->token != g_sdk_command_notified[slot];
}
static int sdk_command_callback_owner(const struct scmd_record *record) {
    int owner = record->owner;
    return owner >= 0 && owner < g_cm_n && record->owner_lifetime == g_sdk_commands.owner_lifetime[owner] &&
           sdk_data_owner_active(owner) && g_sdk_callbacks[owner].command;
}
/* Retained records provide exactly-once completion despite journal overruns.
 * Copy under the lock, then release it before entering mod code. */
static int sdk_command_record_callback(int slot) {
    if (pthread_mutex_trylock(&g_sdk_commands.mu)) return 0;
    const struct scmd_record *record = &g_sdk_commands.records[slot];
    int owner = record->owner;
    int deliver = sdk_command_callback_ready(record, slot) && sdk_command_callback_owner(record);
    az_command_result result;
    if (deliver) {
        result = record->result;
        g_sdk_command_notified[slot] = record->token;
    }
    pthread_mutex_unlock(&g_sdk_commands.mu);
    if (deliver) g_sdk_callbacks[owner].command(&result, g_sdk_callbacks[owner].command_user);
    return 1;
}
static void sdk_data_callbacks_tick(void) {
    for (int owner = 0; owner < g_cm_n; ++owner) sdk_data_owner_callbacks(owner);
    for (int slot = 0; slot < SCMD_SLOTS; ++slot)
        if (!sdk_command_record_callback(slot)) break;
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
