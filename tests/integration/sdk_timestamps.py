"""Shared sample-window timestamp comparison for SDK command runners."""
from fractions import Fraction
import math

BEFORE, EQUAL, AFTER = -1, 0, 1

def compare_timestamps(first, second, *, units_per_second=None,
                       sample_rate=1_000_000_000, window_samples=1):
    """Compare in one clock domain; equality is abs(delta) < N/Fs.

    Defaults describe integer CLOCK_MONOTONIC nanoseconds. Floating inputs
    require explicit units to avoid silently treating seconds as nanoseconds.
    Callers comparing
    source seconds supply units_per_second=1 and the observed source rate.
    Integer products are exact; floats retain their binary value as a rational.
    Approximate equality is not transitive and must not be used to sort.
    """
    if units_per_second is None:
        if type(first) is float or type(second) is float:
            raise ValueError('explicit timestamp units required for floating clocks')
        units_per_second = 1_000_000_000
    if any(type(value) is not int or value <= 0 for value in
           (units_per_second, sample_rate, window_samples)):
        raise ValueError('positive timestamp units, sample rate and window required')
    if any(type(value) not in (int, float) or
           (type(value) is float and not math.isfinite(value)) for value in (first, second)):
        raise ValueError('finite numeric timestamps required')
    difference = abs(Fraction(first) - Fraction(second))
    if difference * sample_rate < window_samples * units_per_second:
        return EQUAL
    return BEFORE if first < second else AFTER
