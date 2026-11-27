/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TA_PHRASE_MODEL_FIXTURE_H
#define TA_PHRASE_MODEL_FIXTURE_H

#include "phrase.h"
#include <assert.h>

static const double *fixture_phrase_parameters(void)
{
    static double parameters[TA_PHRASE_PARAMETER_COUNT];
    static bool loaded;
    if (!loaded) {
        const ta_request request = {.assets_path = getenv("TA_TEST_MODEL_ASSETS")};
        ta_context *c = calloc(1, sizeof *c);
        assert(c && request.assets_path);
        c->request = &request;
        assert(!setjmp(c->failure));
        ta_model_phrase_parameters(c, parameters);
        ta_cleanup(c);
        free(c);
        loaded = true;
    }
    return parameters;
}

static inline void fixture_windows_reduction(double *order)
{
    const double packed[] = {0x1504, 0x3726, 0xba98, -1.0 - 0xcd};
    memcpy(order, packed, sizeof packed);
}

/* The generated fixture source targets the historical helper signatures. */
#define initial_mood(...) initial_mood(__VA_ARGS__, fixture_phrase_parameters())
#define bpm_flags(...) bpm_flags(__VA_ARGS__, fixture_phrase_parameters())
#define classify_compression(...) classify_compression(__VA_ARGS__, fixture_phrase_parameters())
#define classify_power(...) classify_power(__VA_ARGS__, fixture_phrase_parameters())
#define rhythm_flags(...) rhythm_flags(__VA_ARGS__, fixture_phrase_parameters())
#define slow_tempo(...) slow_tempo(__VA_ARGS__, fixture_phrase_parameters())
#define slow_rock(...) slow_rock(__VA_ARGS__, fixture_phrase_parameters())
#define phrase_candidates(...) phrase_candidates(__VA_ARGS__, fixture_phrase_parameters())
#define phrase_route(...) phrase_route(__VA_ARGS__, fixture_phrase_parameters())
#define decorate_high(...) decorate_high(__VA_ARGS__, fixture_phrase_parameters())
#define fill_cut(...) fill_cut(__VA_ARGS__, fixture_phrase_parameters())
#define opening_judge(...) opening_judge(__VA_ARGS__, fixture_phrase_parameters())
#define opening_main(...) opening_main(__VA_ARGS__, fixture_phrase_parameters())
#define recurrence(...) recurrence(__VA_ARGS__, fixture_phrase_parameters())
#define export_non_high(...) export_non_high(__VA_ARGS__, fixture_phrase_parameters())
#define vocal_entries(...) vocal_entries(__VA_ARGS__, fixture_phrase_parameters())
#define non_high_seeds(...) non_high_seeds(__VA_ARGS__, fixture_phrase_parameters())
#define reconcile_roles(...) reconcile_roles(__VA_ARGS__, fixture_phrase_parameters())
#define high_classify(...) high_classify(__VA_ARGS__, fixture_phrase_parameters())
#define attacks_init(...) attacks_init(__VA_ARGS__, fixture_phrase_parameters())
#define beat_four(...) beat_four(__VA_ARGS__, fixture_phrase_parameters())
#define simple_loops(...) simple_loops(__VA_ARGS__, fixture_phrase_parameters())
#define restore_loops(...) restore_loops(__VA_ARGS__, fixture_phrase_parameters())
#define repeated_scores(...) repeated_scores(__VA_ARGS__, fixture_phrase_parameters())
#define repeated_upper(...) repeated_upper(__VA_ARGS__, fixture_phrase_parameters())
#define clean_activity(...) clean_activity(__VA_ARGS__, fixture_phrase_parameters())
#define phrase_outro(...) phrase_outro(__VA_ARGS__, fixture_phrase_parameters())
#define phrase_variation(...) phrase_variation(__VA_ARGS__, fixture_phrase_parameters())
#define phrase_convert(...) phrase_convert(__VA_ARGS__, fixture_phrase_parameters())
#define decorate_rhythm(...) decorate_rhythm(__VA_ARGS__, fixture_phrase_parameters())
#define fill_growth(...) fill_growth(__VA_ARGS__, fixture_phrase_parameters())
#define low_activity(...) low_activity(__VA_ARGS__, fixture_phrase_parameters())
#define middle_activity(...) middle_activity(__VA_ARGS__, fixture_phrase_parameters())
#define all_activity(...) all_activity(__VA_ARGS__, fixture_phrase_parameters())
#define normalize_high(...) normalize_high(__VA_ARGS__, fixture_phrase_parameters())
#define snare_candidates(...) snare_candidates(__VA_ARGS__, fixture_phrase_parameters())
#define final_adjust(...) final_adjust(__VA_ARGS__, fixture_phrase_parameters())
#define opening_spectra(...) opening_spectra(__VA_ARGS__, fixture_phrase_parameters())
#define opening_features(...) opening_features(__VA_ARGS__, fixture_phrase_parameters())
#define vocal_activity(...) vocal_activity(__VA_ARGS__, fixture_phrase_parameters())
#define spectral_peaks(...) spectral_peaks(__VA_ARGS__, fixture_phrase_parameters())

#endif
