#pragma once

struct bwe_sample {
	int bytes;
	int departure_time;
	int arrival_time; // round-trip arrival time
};

struct bwe* bwe_create(int rtt_min);
void bwe_destroy(struct bwe* self);

void bwe_feed(struct bwe* self, const struct bwe_sample* sample);
void bwe_update_rtt_min(struct bwe* self, int rtt_min);
int bwe_get_estimate(const struct bwe* self);
