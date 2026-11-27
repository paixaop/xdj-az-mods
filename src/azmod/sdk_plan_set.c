/* SPDX-License-Identifier: GPL-3.0-only */
/* The native menu publishes a copied target. The mod receives it on the overlay
 * thread, then resolves the mount to a physical USB port on its worker. */
static _Atomic int plan_set_owner; /* slot + 1 */
#ifndef SDK_PROC_IMPL
static pthread_mutex_t plan_set_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int owner;
    az_plan_set_target_v1 target;
} plan_set_pending;
#endif

static cm_mod *plan_set_running(void)
{
    int owner = atomic_load(&plan_set_owner);
    if (!owner || !cm_running(&g_cm[owner - 1]))
        return NULL;
    return &g_cm[owner - 1];
}

static int sdk_on_plan_set(const char *id, az_plan_set_fn fn, void *user)
{
    cm_mod *c = cm_self(id, CM_RA(), "on_plan_set");
    if (!c || !fn || !g_sdk_in_init)
        return AZ_INVALID;
    int owner = (int)(c - g_cm);
    if (owner != g_cm_loading || !cm_called_by(c, (uintptr_t)fn))
        return AZ_INVALID;
    if (plan_set_running() || c->plan_set_fn)
        return AZ_BUSY;
    c->plan_set_fn = fn;
    c->plan_set_user = user;
    atomic_store(&plan_set_owner, owner + 1);
    return AZ_OK;
}

#ifndef SDK_PROC_IMPL
static int az_plan_set_available(void)
{
    return plan_set_running() != NULL;
}

static int az_plan_set_queue(const az_plan_set_target_v1 *target)
{
    cm_mod *c = plan_set_running();
    if (!c || pthread_mutex_trylock(&plan_set_mutex) != 0)
        return 0;
    int queued = !plan_set_pending.owner;
    if (queued) {
        plan_set_pending.target = *target;
        plan_set_pending.owner = (int)(c - g_cm) + 1;
    }
    pthread_mutex_unlock(&plan_set_mutex);
    return queued;
}

static int plan_set_take(cm_mod *c, az_plan_set_target_v1 *target)
{
    if (pthread_mutex_trylock(&plan_set_mutex) != 0)
        return 0;
    int owner = plan_set_pending.owner;
    if (owner)
        *target = plan_set_pending.target;
    plan_set_pending.owner = 0;
    pthread_mutex_unlock(&plan_set_mutex);
    return owner && c == &g_cm[owner - 1];
}

static void sdk_plan_set_tick(void)
{
    cm_mod *c = plan_set_running();
    az_plan_set_target_v1 target;
    if (plan_set_take(c, &target))
        c->plan_set_fn(&target, c->plan_set_user);
}

static bool plan_set_target_valid(const az_plan_set_target_v1 *target)
{
    return target && target->size >= sizeof *target && target->version == AZ_DATA_VERSION &&
        target->playlist_id && target->mount[0] == '/' &&
        memchr(target->mount, '\0', sizeof target->mount);
}

static bool plan_set_output_valid(const az_plan_set_source_v1 *out)
{
    return out && out->size >= sizeof *out && out->version == AZ_DATA_VERSION;
}

/* The target's mount field is opaque to mods. Carry the complete USB
 * identity through the callback without depending on a native cache slot. */
static bool plan_set_mount_route(const az_plan_set_source_v1 *source, char *out, size_t capacity)
{
    if (!source->source_generation)
        return false;
    int length = snprintf(out, capacity, "/az-plan-set/%u/%016jx/%016jx", source->stick,
                          (uintmax_t)source->source_generation, (uintmax_t)source->library_id);
    return length > 0 && (size_t)length < capacity;
}

static bool plan_set_usb_matches(const char *location, bool is_route, const char *mount,
                                 const az_plan_set_source_v1 *identity)
{
    if (!is_route)
        return !strcmp(location, mount);
    char route[sizeof(((az_plan_set_target_v1 *)0)->mount)];
    return plan_set_mount_route(identity, route, sizeof route) && !strcmp(location, route);
}

static int plan_set_usb(const char *location, bool is_route, az_plan_set_source_v1 *out)
{
    for (int stick = 0; stick < 2; ++stick) {
        struct st_source source;
        if (st_source_read(stick, &source) != AZ_OK)
            continue;
        az_plan_set_source_v1 current = {.size = sizeof current, .version = AZ_DATA_VERSION,
            .stick = (uint32_t)stick, .source_generation = source.ref.source_generation,
            .library_id = source.ref.library_id};
        if (!plan_set_usb_matches(location, is_route, source.mount, &current))
            continue;
        *out = current;
        return AZ_OK;
    }
    return AZ_STALE;
}

static int sdk_plan_set_resolve(const char *id, const az_plan_set_target_v1 *target,
                               az_plan_set_source_v1 *out)
{
    cm_mod *c = cm_self(id, CM_RA(), "plan_set_resolve");
    if (c != plan_set_running() || g_cm_on_ui || !c)
        return AZ_INVALID;
    if (!plan_set_target_valid(target) || !plan_set_output_valid(out))
        return AZ_INVALID;
    return plan_set_usb(target->mount, true, out);
}
#endif
