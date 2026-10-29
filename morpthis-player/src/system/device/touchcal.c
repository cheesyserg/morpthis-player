#include "touchcal.h"

#include <stdio.h>

#include "src/system/core/config.h"

// The seam as measured on the V1's Hynitron panel: the coordinate holds at
// about 140 from below, jumps to about 166, and holds there from above.
// device_config.ini replaces these once the calibration page has run.
#define DEFAULT_LOW 140
#define DEFAULT_HIGH 166

// A band is plausible if its width is in this range. The V1 measures 26; the
// bounds are wide enough for another panel and narrow enough that a typo in
// the config is not mistaken for one.
#define CAL_MIN_GAP 10
#define CAL_MAX_GAP 60

// What the session records: a crossing is a jump of 16..48 counts that crosses
// the middle row of the panel. Ordinary fast motion at the turn of a swipe
// stays on one side of that row and is not recorded, and the test does not
// depend on the stored values, so a drifted seam can still be re-measured.
#define CAL_MIN_STEP 16
#define CAL_MAX_STEP 48
#define CAL_SIDE_CLUSTERS 3
#define CAL_TOLERANCE 5
#define CAL_MIN_PER_SIDE 2
#define CAL_MIN_SAMPLES (2 * CAL_MIN_PER_SIDE)

// Section and keys in device_config.ini. Kept out of the app's UI settings so
// a hand edit is obvious.
#define CAL_SECTION "touch"
#define CAL_KEY_LOW "seam_lo"
#define CAL_KEY_HIGH "seam_hi"

// The held side of a crossing is the one stable value: the panel has been
// sitting on it, and the jump leaves from it.  The far side carries the
// finger's overshoot and varies with speed, so it is not recorded.  A down
// crossing (delta > 0) leaves the low plateau; an up crossing leaves the high
// one.  Each side gets its own cluster list and the densest cluster wins.
typedef struct {
	int32_t sum;
	uint32_t count;
} side_cluster_t;

static bool loaded;
static int32_t low = DEFAULT_LOW;
static int32_t high = DEFAULT_HIGH;

static bool active;
static int32_t extent = 319;
static bool have_prev;
static int32_t prev;
static side_cluster_t low_side[CAL_SIDE_CLUSTERS];
static side_cluster_t high_side[CAL_SIDE_CLUSTERS];
static int low_clusters;
static int high_clusters;

static bool plausible(int32_t lo, int32_t hi) {
	int32_t gap = hi - lo;
	return lo >= 0 && hi <= 4095 && gap >= CAL_MIN_GAP && gap <= CAL_MAX_GAP;
}

void touchcal_init(void) {
	if (loaded) {
		return;
	}
	loaded = true;

	long lo = config_get_int(CAL_SECTION, CAL_KEY_LOW, DEFAULT_LOW);
	long hi = config_get_int(CAL_SECTION, CAL_KEY_HIGH, DEFAULT_HIGH);
	if (lo < 0 || hi < 0 || lo > 4095 || hi > 4095 || hi - lo < CAL_MIN_GAP || hi - lo > CAL_MAX_GAP) {
		lo = DEFAULT_LOW;
		hi = DEFAULT_HIGH;
	}
	low = (int32_t)lo;
	high = (int32_t)hi;
}

int32_t touchcal_low(void) {
	touchcal_init();
	return low;
}

int32_t touchcal_high(void) {
	touchcal_init();
	return high;
}

int32_t touchcal_gap(void) {
	touchcal_init();
	return high - low;
}

// The crossing test looks for steps that leave one side of the band and arrive
// beyond the other. The edges sit a quarter of the band inside each plateau:
// 146..160 on the defaults, which is below every observed overshoot and above
// every held value.
int32_t touchcal_band_lo(void) {
	touchcal_init();
	return low + (high - low) / 4;
}

int32_t touchcal_band_hi(void) {
	touchcal_init();
	return high - (high - low) / 4;
}

int32_t touchcal_min_step(void) {
	touchcal_init();
	int32_t half = (high - low) / 2;
	return half > CAL_MIN_STEP ? half : CAL_MIN_STEP;
}

// A tap is placed at the middle of the band when it lands within this many
// counts of a held value. A sixth of the band covers the held values seen on
// the panel and is small enough not to capture ordinary taps.
int32_t touchcal_tap_snap(void) {
	touchcal_init();
	int32_t quarter = (high - low) / 6;
	return quarter > 3 ? quarter : 3;
}

void touchcal_begin(int32_t rows) {
	active = true;
	extent = rows > 0 ? rows : 319;
	have_prev = false;
	low_clusters = 0;
	high_clusters = 0;
}

void touchcal_end(void) { active = false; }

bool touchcal_active(void) { return active; }

// Adds `value` to the densest cluster within tolerance, or starts a new one.
// The held plateau repeats at the same value; ordinary fast reports land
// elsewhere and die as one-sample clusters.
static void add_sample(side_cluster_t *clusters, int *count, int32_t value) {
	for (int i = 0; i < *count; i++) {
		int32_t avg = clusters[i].sum / (int32_t)clusters[i].count;
		int32_t d = value - avg;
		if (d < 0) {
			d = -d;
		}
		if (d <= CAL_TOLERANCE) {
			clusters[i].sum += value;
			clusters[i].count++;
			return;
		}
	}
	if (*count < CAL_SIDE_CLUSTERS) {
		clusters[*count].sum = value;
		clusters[*count].count = 1;
		(*count)++;
	}
}

static int best_cluster(const side_cluster_t *clusters, int count) {
	int best = -1;
	for (int i = 0; i < count; i++) {
		if (best < 0 || clusters[i].count > clusters[best].count) {
			best = i;
		}
	}
	return best;
}

void touchcal_feed(int32_t y, bool pressed) {
	if (!active) {
		return;
	}
	if (!pressed) {
		have_prev = false;
		return;
	}
	if (!have_prev) {
		have_prev = true;
		prev = y;
		return;
	}

	int32_t from = prev;
	prev = y;

	int32_t delta = y - from;
	int32_t step = delta < 0 ? -delta : delta;
	if (step < CAL_MIN_STEP || step > CAL_MAX_STEP) {
		return;
	}

	int32_t middle = extent / 2;
	if (!((from < middle && y > middle) || (from > middle && y < middle))) {
		return;
	}

	if (delta > 0) {
		add_sample(low_side, &low_clusters, from);
	} else {
		add_sample(high_side, &high_clusters, from);
	}
}

uint32_t touchcal_samples(void) {
	int li = best_cluster(low_side, low_clusters);
	int hi = best_cluster(high_side, high_clusters);
	uint32_t l = li < 0 ? 0 : low_side[li].count;
	uint32_t h = hi < 0 ? 0 : high_side[hi].count;
	return 2 * (l < h ? l : h);
}

uint32_t touchcal_needed(void) { return CAL_MIN_SAMPLES; }

bool touchcal_result(int32_t *low_out, int32_t *high_out, int32_t *gap_out) {
	int li = best_cluster(low_side, low_clusters);
	int hi = best_cluster(high_side, high_clusters);
	if (li < 0 || hi < 0 || low_side[li].count < CAL_MIN_PER_SIDE || high_side[hi].count < CAL_MIN_PER_SIDE) {
		return false;
	}

	int32_t lo = low_side[li].sum / (int32_t)low_side[li].count;
	int32_t hi_v = high_side[hi].sum / (int32_t)high_side[hi].count;
	if (!plausible(lo, hi_v)) {
		return false;
	}
	if (low_out) {
		*low_out = lo;
	}
	if (high_out) {
		*high_out = hi_v;
	}
	if (gap_out) {
		*gap_out = hi_v - lo;
	}
	return true;
}

bool touchcal_apply(int32_t lo, int32_t hi) {
	if (!plausible(lo, hi)) {
		return false;
	}

	low = lo;
	high = hi;
	config_set_int(CAL_SECTION, CAL_KEY_LOW, lo);
	config_set_int(CAL_SECTION, CAL_KEY_HIGH, hi);
	if (!config_save()) {
		// The values are in use for this session; say so where a developer
		// looking at the log will see why they did not survive a reboot.
		const char *path = config_path();
		fprintf(stderr, "touchcal: %s could not be written\n", path ? path : "the config");
	}
	return true;
}

void touchcal_defaults(void) { touchcal_apply(DEFAULT_LOW, DEFAULT_HIGH); }
