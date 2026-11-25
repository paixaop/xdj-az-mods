/* SPDX-License-Identifier: GPL-3.0-only */
static bool playlist_page_valid(uint32_t playlist, const az_playlist_entry_v1 *entries,
                                uint32_t capacity, const az_playlist_order_v1 *out)
{
    return playlist && capacity <= AZ_PLAYLIST_PAGE_LIMIT && (!capacity || entries) &&
           out && out->size >= sizeof *out && out->version == AZ_DATA_VERSION;
}

static int sdk_playlist_order(const char *id, int stick, uint32_t playlist, uint32_t offset,
                              az_playlist_entry_v1 *entries, uint32_t capacity, az_playlist_order_v1 *out)
{
    if (st_mod_owner(id, CM_RA(), "playlist_order") < 0 || g_cm_on_ui ||
        !playlist_page_valid(playlist, entries, capacity, out))
        return AZ_INVALID;
    if (!sdk_sq_resolve())
        return AZ_UNSUPPORTED;
    return playlist_snapshot(stick, playlist, offset, entries, capacity, out);
}

static int sdk_playlist_move(const char *id, const az_playlist_move_v1 *request, uint64_t *receipt)
{
    if (st_mod_owner(id, CM_RA(), "playlist_move") < 0 || g_cm_on_ui)
        return AZ_INVALID;
    return playlist_queue(id, request, receipt);
}

static int sdk_playlist_move_status(const char *id, uint64_t receipt, az_playlist_move_result_v1 *out)
{
    if (st_mod_owner(id, CM_RA(), "playlist_move_status") < 0)
        return AZ_INVALID;
    return playlist_poll(id, receipt, out);
}
