/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by the storage worker. The native side retains these witnesses
 * with the original parsed FileData/control, across all getter families. */
static int fi_path_valid(const char *path, size_t bytes, const uint16_t *audio, size_t units) {
    if (!path || !bytes || bytes >= 1024 || memchr(path, 0, bytes)) return 0;
    return (!audio && !units) || fo_unicode(audio, units);
}
static int fi_source_paths(const char *actual, size_t bytes, char root[1024], char analysis[1024]) {
    unsigned found = 0;
    for (unsigned port = 0; port < 2; port++) {
        struct st_source source;
        if (st_source_read(port, &source)) continue;
        size_t mount = strlen(source.mount);
        if (bytes <= mount || memcmp(actual, source.mount, mount) || actual[mount] != '/') continue;
        if (mount + sizeof "/PIONEER" > 1024) return 0;
        memcpy(root, actual, mount);
        memcpy(root + mount, "/PIONEER", sizeof "/PIONEER");
        memcpy(analysis, actual + mount, bytes - mount + 1);
        found++;
    }
    return found == 1;
}
static unsigned fi_extension(char *analysis) {
    char *extension = strrchr(analysis, '.');
    if (!extension) return 0;
    unsigned required;
    if (!strcmp(extension, ".DAT")) required = 1;
    else if (!strcmp(extension, ".EXT")) required = 2;
    else return 0;
    memcpy(extension, ".DAT", 5);
    return required;
}
static uint64_t fi_begin(const char *path, size_t bytes, const uint16_t *audio, size_t units) {
    if (!fi_path_valid(path, bytes, audio, units)) return 0;
    char actual[1024], root[1024], analysis[1024];
    memcpy(actual, path, bytes);
    actual[bytes] = 0;
    if (!fi_source_paths(actual, bytes, root, analysis)) return 0;
    unsigned required = fi_extension(analysis);
    if (!required) return 0;
    struct fo_read *read = fo_alloc();
    if (!read) return 0;
    read->input_mask = required;
    unsigned file = required == 1 ? 0u : 1u;
    if (!fo_open(read, root, strlen(root), audio, units, NULL, 0, analysis, required) ||
        read->files[file].rc || strcmp(read->files[file].path, actual)) {
        fo_drop(read);
        return 0;
    }
    return fo_insert(read);
}

static int fi_framed(const uint8_t *data, size_t size) {
    if (size > ST_FILE_MAX || !fo_container(data, size)) return 0;
    for (size_t at = hc_be32(data + 4); at < size;) {
        if (size - at < 12) return 0;
        uint32_t header = hc_be32(data + at + 4), total = hc_be32(data + at + 8);
        if (header < 12 || header > total || total > size - at) return 0;
        at += total;
    }
    return 1;
}

static int fi_seal_eligible(struct fo_read *read, int original_success, unsigned mask) {
    return original_success && (mask == 1 || mask == 2) && read->opened == mask && fo_version_current(read) && fo_alive_files(read, mask);
}
static int fi_seal_read(struct fo_read *read, int original_success) {
    unsigned mask = read->input_mask, file = mask == 1 ? 0u : 1u;
    if (!fi_seal_eligible(read, original_success, mask)) return 0;
    const struct ss_file *input = &read->files[file];
    size_t size = 0;
    uint8_t *data = st_read_file_checked(input->path, &size, input->device, input->inode, input->signature);
    int valid = fi_framed(data, size) && fo_version_current(read) && fo_alive_files(read, mask);
    if (valid) azsha256_hex(data, size, read->input_digest[file]);
    free(data);
    return valid;
}
static uint64_t fi_seal_token(struct fo_read *read, unsigned slot, int valid) {
    uint64_t token = 0;
    pthread_mutex_lock(&ss_mu);
    uint64_t serial = valid ? fo_new_id() : 0;
    if (serial) {
        read->kind = 3;
        read->count = read->complete = 1;
        token = read->token[0] = (serial << 6) | slot;
        atomic_store_explicit(&fo_custody[slot].tokens[0], token, memory_order_release);
        /* Initial custody is transferred to the native FileData registration. */
        atomic_store_explicit(&fo_custody[slot].state, (read->id << 16) | 1, memory_order_release);
    } else {
        fo_reads[slot] = NULL;
        atomic_store_explicit(&fo_custody[slot].state, 0, memory_order_release);
    }
    pthread_mutex_unlock(&ss_mu);
    return token;
}
static uint64_t fi_seal(uint64_t id, int original_success) {
    unsigned slot;
    struct fo_read *read = fo_claim_read(id, &slot);
    if (!read) return 0;
    int valid = fi_seal_read(read, original_success);
    if (read->db) ss_close_database(read->db, read->db_fd);
    read->db = NULL;
    uint64_t token = fi_seal_token(read, slot, valid);
    if (!token) fo_drop(read);
    return token;
}

/* Called under ss_mu. The native caller separately proves that this exact
 * token still belongs to the borrowed original FileData/control pair. */
static int fi_token_matches(const struct fo_read *read, uint64_t token, unsigned file, unsigned slot) {
    return read && read->kind == 3 && read->complete == 1 && read->count == 1 &&
           read->token[0] == token && read->input_mask == (1u << file) &&
           !atomic_load_explicit(&fo_custody[slot].lost, memory_order_acquire) &&
           atomic_load_explicit(&fo_custody[slot].state, memory_order_acquire) >> 16 == read->id;
}
static struct fo_read *fi_token(uint64_t token, unsigned file) {
    if (!token) return NULL;
    unsigned slot = (unsigned) (token & 63u);
    struct fo_read *read = fo_reads[slot];
    if (!fi_token_matches(read, token, file, slot)) return NULL;
    if (!fo_alive_files(read, read->input_mask)) {
        read->count = 0;
        return NULL;
    }
    return read;
}

static int fi_inherit_input(struct fo_read *read, const struct fo_read *original, unsigned file) {
    if (!original) return 0;
    if (read->opened && (!st_resolution_source_equal(&read->source, &original->source) || strcmp(read->audio, original->audio))) return 0;
    if (!read->opened) {
        read->source = original->source;
        memcpy(read->audio, original->audio, sizeof read->audio);
        memcpy(read->files, original->files, sizeof read->files);
    }
    read->files[file] = original->files[file];
    memcpy(read->input_digest[file], original->input_digest[file], sizeof read->input_digest[file]);
    read->opened |= 1u << file;
    return 1;
}
static int fi_inherit_sources(struct fo_read *read, const uint64_t tokens[2]) {
    int valid = 1;
    pthread_mutex_lock(&ss_mu);
    for (unsigned file = 0; file < 2 && valid; file++) {
        if (!tokens[file]) continue;
        valid = fi_inherit_input(read, fi_token(tokens[file], file), file);
    }
    if (valid) fo_hold(tokens, 2, 1);
    pthread_mutex_unlock(&ss_mu);
    return valid;
}
static int fi_install_watch(struct fo_read *read, const uint64_t tokens[2]) {
    /* An absent participant supplies no origin. Its current metadata only
     * guards selection against a new overriding companion in the public view. */
    struct st_resolution row = {0};
    memcpy(row.analysis, read->files[0].relative, sizeof row.analysis);
    for (unsigned file = 0; file < 2; file++)
        if (!tokens[file]) ss_file_probe(&read->source, &row, file, &read->files[file]);
    ss_arm_paths(&read->watch, read->files);
    /* Install the receiving watch before checking the original watches again.
     * This closes the handoff interval even if file bytes later repeat. */
    int valid = 1;
    pthread_mutex_lock(&ss_mu);
    for (unsigned file = 0; file < 2 && valid; file++)
        if (tokens[file] && !fi_token(tokens[file], file)) valid = 0;
    pthread_mutex_unlock(&ss_mu);
    valid = valid && fo_alive(read);
    fo_hold(tokens, 2, 0);
    return valid;
}
static uint64_t fi_join(const uint64_t inputs[2]) {
    if (!inputs || (!inputs[0] && !inputs[1])) return 0;
    uint64_t tokens[2] = {inputs[0], inputs[1]};
    struct fo_read *read = fo_alloc();
    if (!read) return 0;
    if (!fi_inherit_sources(read, tokens) || !fi_install_watch(read, tokens)) {
        fo_drop(read);
        return 0;
    }
    read->inherited = 1;
    return fo_insert(read);
}

static const struct azn_file_input_api *fi_api(void) {
    static const struct azn_file_input_api api = {sizeof api, 1, fi_begin, fo_opened, fi_seal, fo_hold, fi_join};
    return &api;
}
