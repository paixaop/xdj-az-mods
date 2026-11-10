/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared Anlz FileData inception. Its stock cache is used by cues, grids and
 * phrase getters alike. A later getter inherits the original physical read;
 * it cannot relabel cached bytes with the file now occupying the same path. */
static const azn_file_input_api *file_inputs_api() {
    const auto cb = file_callbacks.load(std::memory_order_acquire);
    if (!cb || cb->size < offsetof(azn_file_cue_callbacks, inputs) + sizeof cb->inputs || !cb->inputs) return nullptr;
    const auto api = cb->inputs();
    return api && api->version == 1 && api->size >= sizeof *api && api->physical_begin &&
                   api->opened && api->seal && api->hold && api->join ? api : nullptr;
}

/* Retain only the GNU weak control count. The native cache/borrowed getter
 * supplies the strong reference when an identity is used. Keeping the control
 * alive prevents address reuse without retaining the parsed file payload. */
static bool file_input_weak_acquire(FileAtomOwner &out, uintptr_t object, uintptr_t control) {
    if (!ptr(object) || !ptr(control) || __atomic_load_n(reinterpret_cast<int32_t *>(control + 8), __ATOMIC_ACQUIRE) <= 0)
        return false;
    auto count = reinterpret_cast<int32_t *>(control + 12);
    int32_t value = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    for (unsigned attempt = 0; attempt < 8 && value > 0 && value < INT32_MAX; ++attempt) {
        if (__atomic_compare_exchange_n(count, &value, value + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            out.object = object;
            out.control = control;
            return true;
        }
    }
    return false;
}
static void file_input_weak_release(FileAtomOwner &owner) {
    const uintptr_t control = owner.control;
    owner = {};
    if (control && !__atomic_sub_fetch(reinterpret_cast<int32_t *>(control + 12), 1, __ATOMIC_ACQ_REL))
        reinterpret_cast<void (*)(uintptr_t)>(object_word(object_word(control) + 24))(control);
}
struct FileInputOrigin {
    FileAtomOwner owner;
    const azn_file_input_api *api = nullptr;
    uint64_t token = 0;
    unsigned mask = 0;
};
static FileInputOrigin file_input_origins[16];
static unsigned file_input_next;
static std::atomic_flag file_input_busy = ATOMIC_FLAG_INIT;
static void file_input_release(FileInputOrigin &origin) {
    if (origin.token) origin.api->hold(&origin.token, 1, 0);
    file_input_weak_release(origin.owner);
    origin = {};
}
/* Takes the one held reference returned by seal on every path. */
static void file_input_remember(const azn_file_input_api *api, uint64_t token, unsigned mask,
                                uintptr_t object, uintptr_t control) {
    if (!token) return;
    FileInputOrigin next;
    next.api = api;
    next.token = token;
    next.mask = mask;
    if ((mask != 1 && mask != 2) || !file_input_weak_acquire(next.owner, object, control) ||
        file_input_busy.test_and_set(std::memory_order_acquire)) {
        file_input_release(next);
        return;
    }
    unsigned at = file_input_next++ % 16;
    bool duplicate = false;
    for (unsigned i = 0; i < 16; ++i)
        if (file_input_origins[i].owner.object == object && file_input_origins[i].owner.control == control) {
            at = i;
            duplicate = true;
            break;
        }
    FileInputOrigin retired = file_input_origins[at];
    file_input_origins[at] = duplicate ? FileInputOrigin {} : next;
    file_input_busy.clear(std::memory_order_release);
    file_input_release(retired);
    if (duplicate) file_input_release(next);
}
/* The returned token has one borrowed-use hold, independent of eviction. */
static uint64_t file_input_find(const azn_file_input_api *api, uintptr_t object, uintptr_t control, unsigned &mask) {
    mask = 0;
    if (!object || !control || file_input_busy.test_and_set(std::memory_order_acquire)) return 0;
    uint64_t token = 0;
    for (const auto &origin : file_input_origins)
        if (origin.token && origin.api == api && origin.owner.object == object && origin.owner.control == control) {
            token = origin.token;
            mask = origin.mask;
            api->hold(&token, 1, 1);
            break;
        }
    file_input_busy.clear(std::memory_order_release);
    return token;
}
struct FileInputUse {
    const azn_file_input_api *api = file_inputs_api();
    uint64_t tokens[2] {};
    bool seen = false, bad = false;
    void observe(uintptr_t object, uintptr_t control) {
        seen = true;
        if (!api) return;
        unsigned mask = 0;
        uint64_t token = file_input_find(api, object, control, mask);
        if (!token || (mask != 1 && mask != 2)) {
            bad = true;
            if (token) api->hold(&token, 1, 0);
            return;
        }
        unsigned file = mask == 1 ? 0u : 1u;
        if (tokens[file]) {
            if (tokens[file] != token) bad = true;
            api->hold(&token, 1, 0);
        } else tokens[file] = token;
    }
    uint64_t join() const { return api && seen && !bad ? api->join(tokens) : 0; }
    ~FileInputUse() {
        if (api) api->hold(tokens, 2, 0);
    }
};
static thread_local FileInputUse *file_map_inputs = nullptr;
struct FileFreshInput {
    const azn_file_input_api *api = file_inputs_api();
    uint64_t read = 0;
    uintptr_t handle = 0;
    char path[1024] {};
    size_t path_bytes = 0;
    unsigned opens = 0, confirmed = 0, mask = 0;
    bool bad = false;
    ~FileFreshInput() {
        if (read) {
            uint64_t token = api->seal(read, 0);
            if (token) api->hold(&token, 1, 0);
        }
    }
};
static thread_local FileFreshInput *file_fresh_input = nullptr;
static void file_input_start(FileFreshInput &input, uintptr_t handle, uintptr_t path) {
    ++input.opens;
    if (!input.api || input.opens > 2 || !typed(path, file_bias + file_profile->path_table)) {
        input.bad = true;
        return;
    }
    /* FilePathString derives directly from FileId. The admitted CFpHandle
     * opener dynamic_casts that exact type and reads this std::string. */
    const uintptr_t data = object_word(path + 0x10);
    const size_t bytes = object_word(path + 0x18);
    if (data < 0x10000 || data > UINT64_C(0x0000ffffffffffff) || !bytes || bytes >= 1024) {
        input.bad = true;
        return;
    }
    /* The stock FileData factory opens its input, then the parser reopens it
     * before decoding. Keep the inception witness across both opens; every
     * resulting descriptor must still match that original physical edition. */
    if (input.opens > 1) {
        if (input.handle != handle || input.path_bytes != bytes ||
            std::memcmp(input.path,reinterpret_cast<const void *>(data),bytes)) input.bad = true;
        return;
    }
    input.handle = handle;
    input.path_bytes = bytes;
    std::memcpy(input.path,reinterpret_cast<const void *>(data),bytes);
    input.read = input.api->physical_begin(reinterpret_cast<const char *>(data), bytes, nullptr, 0);
}
