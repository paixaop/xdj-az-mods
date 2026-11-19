/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TA_PHRASE_H
#define TA_PHRASE_H
#include "internal.h"
typedef ta_doubles D;
typedef ta_ints I;
typedef ta_bytes B;
typedef struct {
    double bpm;
    int offset, phase;
} Timing;
typedef struct {
    double rate;
    int frames;
    D low, middle, high;
} Samples;
typedef struct {
    int n;
    I high, middle, low;
} Coordinates;
typedef struct {
    D values;
    B changes;
} Activity;
typedef struct {
    D normalized, bars;
} AllActivity;
typedef struct {
    Timing timing;
    Coordinates coordinates;
    int initial;
    D lowLevels, middleLevels, allLevels, lowPeaks, lowPeakChanges;
    D quarters, quarterMean, middlePeaks, eighths;
    Activity lowActivity, middleActivity;
    AllActivity allActivity;
} Beats;
typedef struct {
    D low, middle, high, lowLevel, middleLevel;
} Envelopes;
typedef struct {
    D quarters, peaks, bars;
} Power;
typedef struct {
    ta_phrase_biquad low[4], high[4], delaySmoother, bandSmoother;
    int lowCount, second, delay;
    double previous, maximum;
} Attacks;
typedef struct {
    D lowPower, subPower, allPower, allPeaks, dividedLow, dividedMiddle;
    B first, second, hiHat, hiHatChanges;
    I finalMiddle;
    int zeroData, middleExtent;
} Rhythm;
double d_sum(D a);
double d_sequential(D a);
double d_max(D a);
double d_min(D a);
double d_square(D a);
D d_view(ta_context *c, D a, int start, int end);
D d_copy(ta_context *c, D a);
D d_normalized(ta_context *c, D a);
I i_copy(ta_context *c, I a);
ta_phrase_biquad second_order(double rate, double cutoff, double damping, bool high);
ta_phrase_biquad smoother(double rate, double cutoff);
D filter(ta_context *c, ta_phrase_biquad *sections, int count, D input, bool rectify);
Samples phrase_decode(ta_context *c);
Envelopes phrase_envelopes(ta_context *c, Samples s);
Timing reference_timing(ta_context *c, double bpm, double first, int position, double firstBpm);
Coordinates phrase_coordinates(ta_context *c, Samples s, Timing t);
I stage_coordinates(ta_context *c, int count, double rate, double bpm, int offset);
int nominal_beats(double period, int phase, double seconds);
int activity_start(D samples, double rate, double bpm, int offset);
Activity low_activity(ta_context *c, D source, int phase);
Activity middle_activity(ta_context *c, D source, int phase);
AllActivity all_activity(ta_context *c, D source, int phase);
AllActivity middle_peak_activity(ta_context *c, D source, int count, int phase);
Beats phrase_beats(ta_context *c, Samples s, Timing t);
Power rhythm_power(ta_context *c, ta_phrase_biquad *section, D input, double rate, double bpm,
                   I coordinates, int n, bool low);
Attacks attacks_init(ta_context *c, double rate, bool second);
B attacks_process(ta_context *c, Attacks *a, D input, double rate, double bpm, int offset,
                  int phase, int block, I coordinates, int n, bool integerPeriod);
B attacks_timing(ta_context *c, Attacks *a, D input, double rate, double bpm, int offset, int phase,
                 int block);
Rhythm phrase_rhythm(ta_context *c, Samples s, Timing t, Coordinates coordinates);
typedef struct {
    D *rows;
    int n;
} Spectra;
typedef struct {
    int position;
    double height, width, prominence;
} Peak;
typedef struct {
    Peak *v;
    int n;
} Peaks;
typedef struct {
    B presence, rises;
    D envelope;
} OpeningFeatures;
Peaks spectral_peaks(ta_context *c, D samples);
Spectra phrase_spectra(ta_context *c, Samples samples, Timing timing, Coordinates coordinates,
                       bool recurrence);
Spectra opening_spectra(ta_context *c, D samples, double rate);
OpeningFeatures opening_features(ta_context *c, Spectra spectra, int first, int beats);
D vocal_activity(ta_context *c, Spectra spectra);
I vocal_entries(ta_context *c, D activity);
void spectra_release(ta_context *c, Spectra spectra);
enum { MOOD_HIGH = 1, MOOD_MID = 2, MOOD_LOW = 3 };
typedef struct {
    int slowRock, slowTempo;
    double lowPowerRatio;
    int popRock, alternateRock, sparseKick, fourKick, fadeOut, firstOverride, secondOverride;
    int narrowBPM, broadBPM, typicalGenre;
    double compression;
} MoodInputs;
typedef struct {
    int fourKick, popRock, alternateRock, sparseKick;
} RhythmFlags;
typedef struct {
    double ratio;
    D ratios, powers;
    double ratioMean, powerMean;
} LowPower;
typedef struct {
    double first, second;
} Compression;
typedef struct {
    I values;
    D strengths;
    int count, first;
} SnarePositions;
typedef struct {
    D *scores;
    B *related;
    int n;
    double threshold;
} Recurrence;
typedef struct {
    I roles;
    D level, middle, vocal;
} SeedEvidence;
int initial_mood(MoodInputs x);
void bpm_flags(double bpm, int *narrow, int *broad);
LowPower classify_power(ta_context *c, D low, D full, int beats);
Compression classify_compression(ta_context *c, D low, D full, B rhythm, int phase);
int slow_rock(ta_context *c, B low, B middle);
RhythmFlags rhythm_flags(ta_context *c, B rhythm, double bpm, D powers);
int slow_tempo(ta_context *c, double rate, double bpm, D powers, SnarePositions onsets, I beats,
               int sourceOffset, int analysisOffset);
int fade_out(D low, D full);
int revise_mood(ta_context *c, int mood, int beats, D low, D quarters);
Recurrence recurrence(ta_context *c, Spectra spectra, I boundaries, int phase, int endBeat);
SeedEvidence non_high_seeds(ta_context *c, D middle, D envelope, I boundaries, D vocal, int mood);
I reconcile_roles(ta_context *c, I seeds, Recurrence recurrence);
SnarePositions genre_snares(ta_context *c, Samples samples, Timing timing, int *rhythmType);
I i_sorted(ta_context *c, I a, bool descending);
I i_compact(ta_context *c, I a, bool unique);
I i_join(ta_context *c, I a, I b);
I rank_d(ta_context *c, D a, bool descending);
void i_remove(I *a, int index);
typedef struct {
    I hypotheses;
    int selected;
    bool found;
    I raw;
    D scores;
} Opening;
Opening opening_judge(ta_context *c, D envelope, D low, D middle, D vocal, int phase);
Opening opening_main(ta_context *c, D envelope, D low, D middle, D vocal, int phase, I anchors);
typedef struct {
    I roles;
    bool simple;
} HighRoles;
typedef struct {
    D intro, low, quarterPower;
} BlockMeans;
typedef struct {
    int mood, route;
    double score;
    D positive, negative;
} Route;
HighRoles high_classify(ta_context *, I, D, D);
BlockMeans block_means(ta_context *, I, D, D, D, int);
Route phrase_route(ta_context *, D, int, double, I, I, int);
typedef struct {
    D levels, contrast, direction;
} RatioSeries;
typedef struct {
    RatioSeries lowPeak, lowLevel, middleCoarse, middleBeat;
} CandidateFeatures;
typedef struct {
    D levels, differences;
    I positions, beats, opening;
} Anchors;
typedef struct {
    I beats, roles;
} Candidates;
typedef struct {
    Candidates shortList, longList;
} Alternatives;
RatioSeries ratio_series(ta_context *, D);
CandidateFeatures candidate_features(ta_context *, D, D, D, D, int);
I local_candidates(ta_context *, RatioSeries, int);
I verse_hook_candidates(ta_context *, CandidateFeatures, I, int, int, int);
I short_breaks(ta_context *, D, D, int, int);
I adjust_candidates(ta_context *, I, I, int, int);
Anchors secure_points(ta_context *, D, int);
Alternatives initial_alternatives(ta_context *, Anchors, int);
I noise_changes(ta_context *, D);
typedef struct {
    int start, end;
    bool deep;
} BreakEvidence;
typedef struct {
    int firstAligned, first, secondAligned, second;
} ChangeEvidence;
typedef struct {
    D lowPeaks, low, middle, opening, all, changes;
} VerseProfile;
typedef struct {
    I candidates;
    D scores;
    B flags;
    double mean;
} RhythmRejection;
I low_peak_candidates(ta_context *, int, D, int, B);
BreakEvidence break_evidence(ta_context *, I, int, D, D, D);
ChangeEvidence change_evidence(ta_context *, I, int, D, D, D, D, D);
I irregular_seeds(ta_context *, int, D, int, B, int, BreakEvidence, ChangeEvidence);
double verse_score(ta_context *, int, VerseProfile *, double);
I all_band_candidates(ta_context *, I, int, D, B, B, B, VerseProfile);
I adjust_high_candidates(ta_context *, I, D, D);
RhythmRejection reject_repeated_rhythm(ta_context *, I, B);
I add_genre1(ta_context *, I, I, RatioSeries, I, I, int);
I reconcile_high_seeds(ta_context *, I, I);
I finish_candidates(ta_context *, I, int, I, int, int);
typedef struct {
    D values;
    int beats;
} HighProfile;
typedef struct {
    D low, quarterMean, high, power;
    int phase;
    D quarters;
} FillProfile;
enum { FILL_NONE = 0, FILL_GROWTH = 1, FILL_CUT = 2, FILL_UPPER = 3 };
enum { FILL_ALL, FILL_INTRO, FILL_UP_HOOK };
typedef struct {
    int start, end, route;
} FillDetection;
typedef struct {
    I roles, starts, auxiliary, routes;
} Fills;
HighProfile normalize_high(ta_context *, D, int);
I snare_candidates(ta_context *, D, int);
FillDetection fill_growth(ta_context *, int, int, D);
FillDetection fill_cut(ta_context *, int, int, D, D);
int repeated_peak_score(D);
I repeated_scores(ta_context *, HighProfile);
FillDetection repeated_upper(int, int, I);
Fills attach_fills(ta_context *, I, I, FillProfile, int);
void clean_activity(ta_context *, I, FillProfile, Fills *);
bool is_hook(int);
bool is_up(int);
int undecorated(int);
void final_adjust(ta_context *, I, bool, D, Fills *);
typedef struct {
    I roles, starts;
    D metrics;
} UpAnalysis;
typedef struct {
    I roles, starts, first, second, third;
} RhythmAnalysis;
typedef struct {
    I roles;
    int terminal;
} Outro;
typedef struct {
    I boundaries;
    Fills fills;
    I upStarts, upFirst, upSecond, upThird;
} Decorated;
bool beat_four(ta_context *, int, int, bool, D, D);
UpAnalysis simple_loops(ta_context *, I, I, D, D, D);
I restore_loops(ta_context *, I, I, D);
RhythmAnalysis decorate_rhythm(ta_context *, I, I, B);
Outro phrase_outro(ta_context *, I, I, D, B, D, int);
Decorated decorate_high(ta_context *, I, I, FillProfile, D, D, D, B);
typedef struct {
    int flag;
    double summary;
} Variation;
typedef struct {
    az_analysis_phrase_map_v1 map;
    Variation variation;
    I auxiliary;
} PhraseExport;
Variation phrase_variation(ta_context *, D, I);
PhraseExport phrase_convert(ta_context *, int, Decorated, Variation);
PhraseExport export_non_high(ta_context *, int, I, Spectra, int, int, D, D, D, D, B, D, D);
I phrase_candidates(ta_context *, Beats, Opening, D, D, B, B, double, int);
PhraseExport phrase_classify(ta_context *, Samples, Timing);
#endif
