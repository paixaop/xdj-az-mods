/* SPDX-License-Identifier: GPL-3.0-only */
#include "phrase.h"
static double sum(D a, int start, int end)
{
    if ((unsigned)start > (unsigned)end || (unsigned)end >= (unsigned)a.n)
        return 0;
    return d_sequential((D){ a.v + start, end - start + 1 });
}
static D variation_means(ta_context *c, D envelope, I limits, int endpoint_back)
{
    D means = ta_new_doubles(c, limits.n - 1);
    for (int i = 0; i < means.n; i++) {
        int start = limits.v[i] - endpoint_back, end = limits.v[i + 1] - endpoint_back;
        means.v[i] = end >= start ? sum(envelope, start, end) / (end - start + 1) : 0;
    }
    return means;
}

static double variation_deviation(ta_context *c, D means, I limits, int first, double reference,
                                  const double *parameters)
{
    D deviations = ta_new_doubles(c, means.n);
    deviations.n = 0;
    for (int i = first; i < means.n - 1; i++)
        if (limits.v[i + 1] - limits.v[i] > parameters[TA_VARIATION_LONG_SPAN] ||
            means.v[i] > reference * parameters[TA_VARIATION_REFERENCE_SCALE])
            deviations.v[deviations.n++] = fabs(reference - means.v[i]);
    double summary = deviations.n ? d_sequential(deviations) / deviations.n : 0;
    return summary;
}

Variation phrase_variation(ta_context *c, D envelope, I boundaries, const double *parameters)
{
    if (boundaries.n <= parameters[TA_VARIATION_MIN_BOUNDARIES_EXCLUSIVE])
        return (Variation){ 0 };
    I limits = i_copy(c, boundaries);
    limits.v[limits.n - 1] = ta_min(limits.v[limits.n - 1], envelope.n);
    D means = variation_means(c, envelope, limits, (int)parameters[TA_VARIATION_MEAN_ENDPOINT_BACK]);
    int reference_first = (int)parameters[TA_VARIATION_REFERENCE_START];
    int reference_count = means.n - (int)parameters[TA_VARIATION_REFERENCE_COUNT_BACK];
    double reference = reference_count > 0 ? sum(means, reference_first, reference_count) / reference_count : 0;
    int quiet = (int)parameters[TA_VARIATION_QUIET_MEAN_INDEX];
    bool omit = means.v[quiet] < reference * parameters[TA_VARIATION_QUIET_SECOND_SCALE] &&
                boundaries.n >= parameters[TA_VARIATION_QUIET_SECOND_MIN_BOUNDARIES];
    int first = (int)parameters[TA_VARIATION_DEVIATION_FIRST_ORDINAL] - 1;
    if (omit) {
        int begin = (int)parameters[TA_VARIATION_OMIT_REFERENCE_START];
        int end = boundaries.n - (int)parameters[TA_VARIATION_OMIT_REFERENCE_END_BACK];
        int divisor = boundaries.n - (int)parameters[TA_VARIATION_OMIT_REFERENCE_DIVISOR_BACK];
        reference = sum(means, begin, end) / divisor;
        first += (int)parameters[TA_VARIATION_DEVIATION_OMIT_ADVANCE];
    }
    double threshold = parameters[omit ? TA_VARIATION_OMIT_THRESHOLD : TA_VARIATION_FULL_THRESHOLD];
    double summary = variation_deviation(c, means, limits, first, reference, parameters);
    return (Variation){ summary >= threshold, summary };
}
static int material_kind(int role, I materialRoles, I materialKinds, int *materials, int *next,
                         const double *parameters)
{
    enum { cycle_length = TA_NON_HIGH_MATERIAL_KIND_5 - TA_NON_HIGH_MATERIAL_KIND_0 + 1 };
    int index = 0;
    while (index < *materials && materialRoles.v[index] != role)
        index++;
    if (index == *materials) {
        materialRoles.v[index] = role;
        materialKinds.v[index] = (int)parameters[TA_NON_HIGH_MATERIAL_KIND_0 + (*next)++ % cycle_length];
        (*materials)++;
    }
    return materialKinds.v[index];
}

static int non_high_kind(int role, I materialRoles, I materialKinds, int *materials, int *next,
                         const double *parameters)
{
    if (role == parameters[TA_NON_HIGH_ROLE_INTRO])
        return (int)parameters[TA_NON_HIGH_KIND_INTRO];
    if (role == parameters[TA_NON_HIGH_ROLE_HOOK])
        return (int)parameters[TA_NON_HIGH_KIND_HOOK];
    if (role == parameters[TA_NON_HIGH_ROLE_BRIDGE])
        return (int)parameters[TA_NON_HIGH_KIND_BRIDGE];
    if (role == parameters[TA_NON_HIGH_ROLE_OUTRO])
        return (int)parameters[TA_NON_HIGH_KIND_OUTRO];
    if (role >= parameters[TA_NON_HIGH_MATERIAL_MIN_ROLE])
        return material_kind(role, materialRoles, materialKinds, materials, next, parameters);
    return 0;
}

static void convert_main_family(az_analysis_phrase_v1 *p, int auxiliary, int style, bool fills_allowed,
                                const double *parameters)
{
    p->fill = fills_allowed && auxiliary != parameters[TA_HIGH_EXPORT_MAIN_NO_FILL_AUXILIARY];
    p->variant1 = style == parameters[TA_HIGH_EXPORT_MAIN_BEAT_STYLE];
}

static void convert_build_family(az_analysis_phrase_v1 *p, Decorated d, int i, int style, int role,
                                 const double *parameters)
{
    p->fill = role >= parameters[TA_HIGH_EXPORT_BUILD_FILL_MIN_ROLE] &&
              d.fills.auxiliary.v[i] != parameters[TA_HIGH_EXPORT_BUILD_NO_FILL_AUXILIARY];
    if (style == parameters[TA_HIGH_EXPORT_BUILD_BEAT_STYLE])
        p->variant3 = true;
    else if (style == parameters[TA_HIGH_EXPORT_BUILD_SEQUENCE_STYLE]) {
        p->variant2 = true;
        p->structure = true;
        p->beat2 = d.upFirst.v[i];
        p->beat3 = d.upSecond.v[i];
        p->beat4 = d.upThird.v[i];
    } else if (style == parameters[TA_HIGH_EXPORT_BUILD_LOOP_STYLE]) {
        p->variant2 = true;
        p->beat2 = d.upStarts.v[i];
    }
}

typedef struct {
    int role, style, outro_style;
    enum ta_high_export_handler handler;
} HighRoleExport;

static void convert_high_family(az_analysis_phrase_v1 *p, Decorated d, int i, HighRoleExport role,
                                const double *parameters)
{
    p->kind = (uint32_t)parameters[TA_HIGH_EXPORT_KIND_INTRO + role.handler];
    switch (role.handler) {
    case TA_HIGH_EXPORT_INTRO:
        convert_main_family(p, d.fills.auxiliary.v[i], role.style,
                            role.role >= parameters[TA_HIGH_EXPORT_INTRO_FILL_MIN_ROLE], parameters);
        break;
    case TA_HIGH_EXPORT_HOOK:
        convert_main_family(p, d.fills.auxiliary.v[i], role.style,
                            role.role >= parameters[TA_HIGH_EXPORT_HOOK_FILL_MIN_ROLE], parameters);
        break;
    case TA_HIGH_EXPORT_BREAK:
        p->fill = role.role > parameters[TA_HIGH_EXPORT_BREAK_FILL_ROLE_MIN_EXCLUSIVE];
        break;
    case TA_HIGH_EXPORT_BUILD:
        convert_build_family(p, d, i, role.style, role.role, parameters);
        break;
    case TA_HIGH_EXPORT_OUTRO:
        p->variant1 = role.outro_style == parameters[TA_HIGH_EXPORT_OUTRO_BEAT_STYLE];
        break;
    default:
        break;
    }
}

static HighRoleExport high_role_export(int role, bool decorated, const double *parameters)
{
    double plain_scale = parameters[TA_HIGH_EXPORT_PLAIN_FAMILY_SCALE];
    int plain_divisor = (int)parameters[TA_HIGH_EXPORT_PLAIN_REMAINDER_DIVISOR];
    int family = (int)(role * (decorated ? parameters[TA_HIGH_EXPORT_DECORATED_FAMILY_SCALE] : plain_scale));
    int first = (int)parameters[TA_HIGH_EXPORT_FIRST_FAMILY];
    if (family < first || family - first > parameters[TA_HIGH_EXPORT_LAST_FAMILY_SLOT])
        return (HighRoleExport){ .handler = TA_HIGH_EXPORT_HANDLER_COUNT };
    int slot = family - first;

    int divisor = decorated ? (int)parameters[TA_HIGH_EXPORT_DECORATED_REMAINDER_DIVISOR] : plain_divisor;
    int remainder = role - family * divisor;
    int tens = (int)(remainder * plain_scale);
    return (HighRoleExport){
        .role = role,
        .style = decorated ? remainder - tens * plain_divisor : remainder,
        .outro_style = decorated ? tens : remainder,
        .handler = (enum ta_high_export_handler)parameters[TA_HIGH_EXPORT_DISPATCH_FIRST + slot],
    };
}

static void convert_high_role(az_analysis_phrase_v1 *p, Decorated d, int i, int role, I auxiliary,
                              const double *parameters)
{
    bool decorated = role > parameters[TA_HIGH_EXPORT_DECORATED_ROLE_MIN_EXCLUSIVE];
    if (!decorated)
        auxiliary.v[i] = 0;
    HighRoleExport decoded = high_role_export(role, decorated, parameters);
    if (decoded.handler < TA_HIGH_EXPORT_HANDLER_COUNT)
        convert_high_family(p, d, i, decoded, parameters);
}

PhraseExport phrase_convert(ta_context *c, int mood, Decorated d, Variation variation,
                            const double *parameters)
{
    int n = d.fills.roles.n;
    if (d.boundaries.n != n + 1 || d.fills.auxiliary.n != n || d.upStarts.n != n ||
        d.upFirst.n != n || d.upSecond.n != n || d.upThird.n != n)
        ta_fail(c, TA_FAILED, "phrase conversion dimensions");
    az_analysis_phrase_v1 *entries = ta_alloc(c, n, sizeof *entries);
    I auxiliary = i_copy(c, d.fills.auxiliary), materialRoles = ta_new_ints(c, n),
      materialKinds = ta_new_ints(c, n);
    int materials = 0, next = (int)parameters[TA_NON_HIGH_MATERIAL_FIRST_INDEX];
    for (int i = 0; i < n; i++) {
        az_analysis_phrase_v1 *p = &entries[i];
        p->start_beat = d.boundaries.v[i];
        p->fill_start_beat = d.fills.starts.v[i];
        int role = d.fills.roles.v[i];
        if (mood != MOOD_HIGH) {
            p->kind = non_high_kind(role, materialRoles, materialKinds, &materials, &next, parameters);
            continue;
        }
        convert_high_role(p, d, i, role, auxiliary, parameters);
    }
    return (PhraseExport){ { (uint32_t)mood, (uint32_t)d.boundaries.v[n], (uint32_t)n, 0, entries },
                           variation,
                           auxiliary };
}
PhraseExport export_non_high(ta_context *c, int mood, I b, Spectra spectra, int phase, int end,
                             D middle, D envelope, D vocal, D low, B activity, D quarters,
                             D power, const double *parameters)
{
    if (mood == MOOD_HIGH || !b.n)
        ta_fail(c, TA_FAILED, "non-high export dimensions");
    int n = b.n;
    I roles, temporary;
    D metric;
    Variation variation = { 0 };
    if (n == parameters[TA_NON_HIGH_SINGLE_SECTION_COUNT]) {
        roles = ta_new_ints(c, n);
        temporary = ta_new_ints(c, n);
        for (int i = 0; i < n; i++) {
            roles.v[i] = (int)parameters[TA_NON_HIGH_SINGLE_FINAL_ROLE];
            temporary.v[i] = (int)parameters[TA_NON_HIGH_SINGLE_TEMPORARY_ROLE];
        }
        metric = ta_new_doubles(c, n);
    } else {
        Recurrence recurrent = recurrence(c, spectra, b, phase, end, parameters);
        SeedEvidence seeds = non_high_seeds(c, middle, envelope, b, vocal, mood, parameters);
        roles = reconcile_roles(c, seeds.roles, recurrent, parameters);
        BlockMeans blocks = block_means(c, b, envelope, low, power, phase, parameters);
        temporary = high_classify(c, b, blocks.intro, blocks.low, parameters).roles;
        metric = blocks.low;
        variation = phrase_variation(c, envelope, b, parameters);
    }
    int terminal = phrase_outro(c, b, temporary, low, activity, quarters, phase, parameters).terminal;
    I zeros = ta_new_ints(c, n);
    Fills fills = { roles, zeros, zeros, zeros };
    I ends = i_join(c, b, (I){ &terminal, 1 });
    final_adjust(c, ends, false, metric, &fills, parameters);
    Decorated decorated = { ends, fills, zeros, zeros, zeros, zeros };
    return phrase_convert(c, mood, decorated, variation, parameters);
}
