/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared Anlz FileData inception. Its stock cache is used by cues, grids and
 * phrase getters alike. A later getter inherits the original physical read;
 * it cannot relabel cached bytes with the file now occupying the same path. */
static bool file_inputs_api_valid(const struct azn_file_input_api *api) {
    return api && api->version == 1 && api->size >= sizeof *api && api->physical_begin && api->opened && api->seal && api->hold && api->join;
}
static const struct azn_file_input_api *file_inputs_api() {
    __auto_type cb = atomic_load_explicit(&(file_callbacks), memory_order_acquire);
    if (!cb || cb->size < offsetof(struct azn_file_cue_callbacks, inputs) + sizeof cb->inputs || !cb->inputs) return NULL;
    __auto_type api = cb->inputs();
    return file_inputs_api_valid(api) ? api : NULL;
}

/* Retain only the GNU weak control count. The native cache/borrowed getter
 * supplies the strong reference when an identity is used. Keeping the control
 * alive prevents address reuse without retaining the parsed file payload. */
static bool file_input_weak_acquire(FileAtomOwner *out, uintptr_t object, uintptr_t control) {
    if (!ptr(object) || !ptr(control) || __atomic_load_n(((int32_t *) (control + 8)), __ATOMIC_ACQUIRE) <= 0)
        return false;
    __auto_type count = ((int32_t *) (control + 12));
    int32_t value = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    for (unsigned attempt = 0; attempt < 8 && value > 0 && value < INT32_MAX; ++attempt) {
        if (__atomic_compare_exchange_n(count, &value, value + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            out->object = object;
            out->control = control;
            return true;
        }
    }
    return false;
}
static void file_input_weak_release(FileAtomOwner *owner) {
    const uintptr_t control = owner->control;
    (*owner) = (FileAtomOwner) {0};
    if (control && !__atomic_sub_fetch(((int32_t *) (control + 12)), 1, __ATOMIC_ACQ_REL))
        ((void (*)(uintptr_t))(object_word(object_word(control) + 24)))(control);
}
struct FileInputOrigin {
    FileAtomOwner owner;
    const struct azn_file_input_api *api;
    uint64_t token;
    unsigned mask;
};
static FileInputOrigin file_input_origins[16];
static unsigned file_input_next;
static atomic_flag file_input_busy = ATOMIC_FLAG_INIT;
static void file_input_release(FileInputOrigin *origin) {
    if (origin->token) origin->api->hold(&origin->token, 1, 0);
    file_input_weak_release(&(origin->owner));
    (*origin) = (FileInputOrigin) {0};
}
static bool file_input_duplicate(uintptr_t object, uintptr_t control, unsigned *at) {
    for (unsigned i = 0; i < 16; ++i) {
        if (file_input_origins[i].owner.object == object && file_input_origins[i].owner.control == control) {
            *at = i;
            return true;
        }
    }
    return false;
}
/* Takes the one held reference returned by seal on every path. */
static void file_input_remember(const struct azn_file_input_api *api, uint64_t token, unsigned mask,
                                uintptr_t object, uintptr_t control) {
    if (!token) return;
    FileInputOrigin next = {0};
    next.api = api;
    next.token = token;
    next.mask = mask;
    if ((mask != 1 && mask != 2) || !file_input_weak_acquire(&(next.owner), object, control) ||
        atomic_flag_test_and_set_explicit(&(file_input_busy), memory_order_acquire)) {
        file_input_release(&(next));
        return;
    }
    unsigned at = file_input_next++ % 16;
    bool duplicate = file_input_duplicate(object, control, &at);
    FileInputOrigin retired = file_input_origins[at];
    file_input_origins[at] = duplicate ? (FileInputOrigin) {0} : next;
    atomic_flag_clear_explicit(&(file_input_busy), memory_order_release);
    file_input_release(&(retired));
    if (duplicate) file_input_release(&(next));
}
static bool file_input_origin_matches(const FileInputOrigin *origin, const struct azn_file_input_api *api, uintptr_t object, uintptr_t control) {
    return origin->token && origin->api == api && origin->owner.object == object && origin->owner.control == control;
}
/* The returned token has one borrowed-use hold, independent of eviction. */
static uint64_t file_input_find(const struct azn_file_input_api *api, uintptr_t object, uintptr_t control, unsigned *mask) {
    (*mask) = 0;
    if (!object || !control || atomic_flag_test_and_set_explicit(&(file_input_busy), memory_order_acquire)) return 0;
    uint64_t token = 0;
    for (unsigned origin_index = 0; origin_index < sizeof(file_input_origins) / sizeof(file_input_origins)[0]; ++origin_index) {
        const FileInputOrigin *origin = &(file_input_origins[origin_index]);
        if (file_input_origin_matches(origin, api, object, control)) {
            token = origin->token;
            (*mask) = origin->mask;
            api->hold(&token, 1, 1);
            break;
        }
    }
    atomic_flag_clear_explicit(&(file_input_busy), memory_order_release);
    return token;
}
struct FileInputUse {
    const struct azn_file_input_api *api;
    uint64_t tokens[2];
    bool seen, bad;
};
static void file_inputs_drop(FileInputUse *self) {
    if (self->api) self->api->hold(self->tokens, 2, 0);
}

static void file_inputs_observe(FileInputUse *self, uintptr_t object, uintptr_t control) {
    self->seen = true;
    if (!self->api) return;
    unsigned mask = 0;
    uint64_t token = file_input_find(self->api, object, control, &(mask));
    if (!token || (mask != 1 && mask != 2)) {
        self->bad = true;
        if (token) self->api->hold(&token, 1, 0);
        return;
    }
    unsigned file = mask == 1 ? 0u : 1u;
    if (self->tokens[file]) {
        if (self->tokens[file] != token) self->bad = true;
        self->api->hold(&token, 1, 0);
    } else self->tokens[file] = token;
}

static uint64_t file_inputs_join(const FileInputUse *self) { return self->api && self->seen && !self->bad ? self->api->join(self->tokens) : 0; }
static _Thread_local FileInputUse *file_map_inputs = NULL;
struct FileFreshInput {
    const struct azn_file_input_api *api;
    uint64_t read;
    uintptr_t handle;
    char path[1024];
    size_t path_bytes;
    unsigned opens, confirmed, mask;
    bool bad;
};
static void file_fresh_drop(FileFreshInput *self) {
    if (self->read) {
        uint64_t token = self->api->seal(self->read, 0);
        if (token) self->api->hold(&token, 1, 0);
    }
}

static _Thread_local FileFreshInput *file_fresh_input = NULL;
static bool file_input_path(uintptr_t data, size_t bytes) {
    return data >= 0x10000 && data <= UINT64_C(0x0000ffffffffffff) && bytes && bytes < 1024;
}
static bool file_input_reopened(const FileFreshInput *input, uintptr_t handle, uintptr_t data, size_t bytes) {
    return input->handle == handle && input->path_bytes == bytes && !memcmp(input->path, (const void *) data, bytes);
}
static void file_input_start(FileFreshInput *input, uintptr_t handle, uintptr_t path) {
    ++input->opens;
    if (!input->api || input->opens > 2 || !typed(path, file_bias + file_profile->path_table)) {
        input->bad = true;
        return;
    }
    /* FilePathString derives directly from FileId. The admitted CFpHandle
     * opener dynamic_casts that exact type and reads this string. */
    const uintptr_t data = object_word(path + 0x10);
    const size_t bytes = object_word(path + 0x18);
    if (!file_input_path(data, bytes)) {
        input->bad = true;
        return;
    }
    /* The stock FileData factory opens its input, then the parser reopens it
     * before decoding. Keep the inception witness across both opens; every
     * resulting descriptor must still match that original physical edition. */
    if (input->opens > 1) {
        if (!file_input_reopened(input, handle, data, bytes)) input->bad = true;
        return;
    }
    input->handle = handle;
    input->path_bytes = bytes;
    memcpy(input->path, ((const void *) (data)), bytes);
    input->read = input->api->physical_begin(((const char *) (data)), bytes, NULL, 0);
}
