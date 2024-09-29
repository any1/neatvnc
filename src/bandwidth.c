#include "bandwidth.h"

#include <stdlib.h>
#include <tgmath.h>

#define SAMPLES_MAX 16

// bandwidth estimator
struct bwe {
	int rtt_min;
	int n_samples;
	int index;
	double estimate;
	struct bwe_sample samples[0];
};

struct bwe* bwe_create(int rtt_min)
{
	struct bwe* self = calloc(1, sizeof(*self) + sizeof(*self->samples) *
			SAMPLES_MAX);
	if (!self)
		return NULL;

	return self;
}

void bwe_destroy(struct bwe* self)
{
	free(self);
}

static inline const struct bwe_sample* get_sample(const struct bwe* self, int index)
{
	int head = (self->index + index - self->n_samples + SAMPLES_MAX)
		% SAMPLES_MAX;
	return &self->samples[head];
}

// Under non-congested circumstances, there will be some space between packages
static double estimate_non_congested_bandwidth(const struct bwe* self)
{
	int bytes_total = 0;
	int bw_delay_total = 0;

	for (int i = 0; i < self->n_samples; ++i) {
		const struct bwe_sample* s = get_sample(self, i);

		int rtt = s->arrival_time - s->departure_time;
		int bw_delay = rtt - self->rtt_min;

		bytes_total += s->bytes;
		bw_delay_total += bw_delay;
	}

	return (double)bytes_total / (bw_delay_total * 1e-6);
}

// Under congested circumstances, there will be no space between packages
static double estimate_congested_bandwidth(const struct bwe* self)
{
	if (self->n_samples == 0)
		return 0;

	const struct bwe_sample* s0 = get_sample(self, 0);
	const struct bwe_sample* s1 = get_sample(self, self->n_samples - 1);

	int bytes_total = 0;

	for (int i = 0; i < self->n_samples; ++i) {
		const struct bwe_sample* s = get_sample(self, i);
		bytes_total += s->bytes;
	}

	int rtt = s1->arrival_time - s0->departure_time;
	int bw_delay = rtt - self->rtt_min;

	return (double)bytes_total / (bw_delay * 1e-6);
}

static void update_estimate(struct bwe* self)
{
	double non_congested = estimate_non_congested_bandwidth(self);
	double congested = estimate_congested_bandwidth(self);
	self->estimate = fmax(non_congested, congested);
}

void bwe_feed(struct bwe* self, const struct bwe_sample* sample)
{
	self->samples[self->index] = *sample;
	self->index = (self->index + 1) % SAMPLES_MAX;

	if (self->n_samples < SAMPLES_MAX)
		self->n_samples++;

	update_estimate(self);
}

void bwe_update_rtt_min(struct bwe* self, int rtt_min)
{
	self->rtt_min = rtt_min;
}

int bwe_get_estimate(const struct bwe* self)
{
	return round(self->estimate);
}
