#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include "lv2/core/lv2.h"

#define LOOPER_URI "http://example.org/hybrid-looper"
#define MAX_BEATS 8.0f
#define DEFAULT_BPM 120.0f

typedef enum {
    STATE_IDLE      = 0,
    STATE_RECORDING = 1,
    STATE_LOOPING   = 2
} PluginState;

typedef struct {
    // Port pointers
    const float* time_beats;
    const float* feedback;
    const float* activator;
    const float* wet_dry;
    const float* in_l;
    const float* in_r;
    float*       out_l;
    float*       out_r;

    // Buffer memory
    float*       buffer_l;
    float*       buffer_r;
    uint32_t     buffer_capacity;
    
    // State machine trackers
    PluginState  state;
    double       sample_rate;
    uint32_t     write_ptr;
    uint32_t     read_ptr;
    uint32_t     total_recorded;
    uint32_t     target_samples;
    uint32_t     max_8_beat_samples;
} HybridLooper;

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate, const char* bundle_path, const LV2_Feature* const* features) {
    HybridLooper* self = (HybridLooper*)calloc(1, sizeof(HybridLooper));
    self->sample_rate = rate;
    self->state = STATE_IDLE;

    // Pre-allocate buffer for 8 beats at an assumed minimum tempo of 60 BPM
    // 60 BPM -> 1 beat = 1 second. 8 beats = 8 seconds.
    self->buffer_capacity = (uint32_t)(rate * 8.0); 
    self->buffer_l = (float*)calloc(self->buffer_capacity, sizeof(float));
    self->buffer_r = (float*)calloc(self->buffer_capacity, sizeof(float));

    return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void* data) {
    HybridLooper* self = (HybridLooper*)instance;
    switch (port) {
        case 0: self->time_beats = (const float*)data; break;
        case 1: self->feedback = (const float*)data; break;
        case 2: self->activator = (const float*)data; break;
        case 3: self->wet_dry = (const float*)data; break;
        case 4: self->in_l = (const float*)data; break;
        case 5: self->in_r = (const float*)data; break;
        case 6: self->out_l = (float*)data; break;
        case 7: self->out_r = (float*)data; break;
    }
}

static void
run(LV2_Handle instance, uint32_t sample_count) {
    HybridLooper* self = (HybridLooper*)instance;

    float act = *(self->activator);
    float mix = *(self->wet_dry);
    float fb  = *(self->feedback);
    float beat_setting = *(self->time_beats);

    // Hardcode 120 BPM for now (In future, we can read this from Mixxx's host port)
    float bpm = DEFAULT_BPM; 
    double samples_per_beat = (60.0 / bpm) * self->sample_rate;

    self->target_samples = (uint32_t)(samples_per_beat * beat_setting);
    self->max_8_beat_samples = (uint32_t)(samples_per_beat * MAX_BEATS);

    // Constrain safety limits
    if (self->target_samples > self->buffer_capacity) self->target_samples = self->buffer_capacity;
    if (self->max_8_beat_samples > self->buffer_capacity) self->max_8_beat_samples = self->buffer_capacity;

    for (uint32_t i = 0; i < sample_count; ++i) {
        float in_left = self->in_l[i];
        float in_right = self->in_r[i];

        // State Transitions
        if (self->state == STATE_IDLE && act >= 0.5f) {
            // Trigger hit -> Transition to recording
            self->state = STATE_RECORDING;
            self->write_ptr = 0;
            self->read_ptr = 0;
            self->total_recorded = 0;
        } else if (act < 0.5f) {
            // Turned off -> Go back to idle
            self->state = STATE_IDLE;
        }

        // Processing based on State
        if (self->state == STATE_IDLE) {
            // Output dry signals directly
            self->out_l[i] = in_left;
            self->out_r[i] = in_right;

        } else if (self->state == STATE_RECORDING) {
            // Record and play dry
            self->buffer_l[self->write_ptr] = in_left;
            self->buffer_r[self->write_ptr] = in_right;
            self->write_ptr++;
            self->total_recorded++;

            self->out_l[i] = in_left;
            self->out_r[i] = in_right;

            // Once we reach the target time, switch to looping
            if (self->total_recorded >= self->target_samples) {
                self->state = STATE_LOOPING;
            }

        } else if (self->state == STATE_LOOPING) {
            // 1. Playback the loop
            float loop_l = self->buffer_l[self->read_ptr];
            float loop_r = self->buffer_r[self->read_ptr];

            self->read_ptr++;
            if (self->read_ptr >= self->target_samples) {
                self->read_ptr = 0;
                // Apply feedback decay directly to the buffer contents for next pass
                for (uint32_t k = 0; k < self->target_samples; ++k) {
                    self->buffer_l[k] *= fb;
                    self->buffer_r[k] *= fb;
                }
            }

            // 2. Perform background recording up to 8 beats
            if (self->total_recorded < self->max_8_beat_samples) {
                self->buffer_l[self->write_ptr] = in_left;
                self->buffer_r[self->write_ptr] = in_right;
                self->write_ptr++;
                self->total_recorded++;
            }

            // 3. Output the mixed Wet/Dry signals
            float wet_gain = mix;
            float dry_gain = 1.0f - mix;

            self->out_l[i] = (in_left * dry_gain) + (loop_l * wet_gain);
            self->out_r[i] = (in_right * dry_gain) + (loop_r * wet_gain);
        }
    }
}

static void cleanup(LV2_Handle instance) {
    HybridLooper* self = (HybridLooper*)instance;
    free(self->buffer_l);
    free(self->buffer_r);
    free(self);
}

static const LV2_Descriptor descriptor = {
    LOOPER_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}
