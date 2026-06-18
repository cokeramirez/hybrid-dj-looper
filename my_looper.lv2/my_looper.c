#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include "lv2/core/lv2.h"

#define LOOPER_URI "http://example.org/hybrid-looper"
#define MAX_BEATS 8.0f

typedef enum {
    STATE_IDLE      = 0,
    STATE_RECORDING = 1,
    STATE_LOOPING   = 2
} PluginState;

typedef struct {
    // Port pointers
    const float* time_step;
    const float* feedback;
    const float* activator;
    const float* wet_dry;
    const float* hpf_enable;
    const float* bpm;
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

    // HPF Filter State Variables (1st-order IIR)
    float        hpf_x1_l;
    float        hpf_y1_l;
    float        hpf_x1_r;
    float        hpf_y1_r;

    // Metrónomo de prueba
    uint32_t     beat_sample_counter;
} HybridLooper;

static const float BEAT_VALUES[9] = {
    0.03125f, // 0: 1/32
    0.0625f,  // 1: 1/16
    0.125f,   // 2: 1/8
    0.25f,    // 3: 1/4
    0.5f,     // 4: 1/2
    1.0f,     // 5: 1 Beat
    2.0f,     // 6: 2 Beats
    4.0f,     // 7: 4 Beats
    8.0f      // 8: 8 Beats
};

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate, const char* bundle_path, const LV2_Feature* const* features) {
    HybridLooper* self = (HybridLooper*)calloc(1, sizeof(HybridLooper));
    self->sample_rate = rate;
    self->state = STATE_IDLE;

    // Reservar memoria para 15 segundos máximo
    self->buffer_capacity = (uint32_t)(rate * 15.0); 
    self->buffer_l = (float*)calloc(self->buffer_capacity, sizeof(float));
    self->buffer_r = (float*)calloc(self->buffer_capacity, sizeof(float));

    self->beat_sample_counter = 0;

    return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void* data) {
    HybridLooper* self = (HybridLooper*)instance;
    switch (port) {
        case 0: self->time_step = (const float*)data; break;
        case 1: self->feedback = (const float*)data; break;
        case 2: self->activator = (const float*)data; break;
        case 3: self->wet_dry = (const float*)data; break;
        case 4: self->hpf_enable = (const float*)data; break;
        case 5: self->bpm = (const float*)data; break;
        case 6: self->in_l = (const float*)data; break;
        case 7: self->in_r = (const float*)data; break;
        case 8: self->out_l = (float*)data; break;
        case 9: self->out_r = (float*)data; break;
    }
}

static void
activate(LV2_Handle instance) {
    HybridLooper* self = (HybridLooper*)instance;
    self->state = STATE_IDLE;
    self->write_ptr = 0;
    self->read_ptr = 0;
    self->total_recorded = 0;
    self->beat_sample_counter = 0;
    
    for (uint32_t i = 0; i < self->buffer_capacity; ++i) {
        self->buffer_l[i] = 0.0f;
        self->buffer_r[i] = 0.0f;
    }

    self->hpf_x1_l = 0.0f; self->hpf_y1_l = 0.0f;
    self->hpf_x1_r = 0.0f; self->hpf_y1_r = 0.0f;
}

static void
run(LV2_Handle instance, uint32_t sample_count) {
    HybridLooper* self = (HybridLooper*)instance;

    float act = *(self->activator);
    float mix = *(self->wet_dry);
    float fb  = *(self->feedback);
    float hpf = *(self->hpf_enable);
    
    float bpm = *(self->bpm);
    if (bpm < 40.0f || bpm > 250.0f) {
        bpm = 120.0f; 
    }

    // Mapear knob de tiempo (0.0 - 1.0) a los 9 pasos
    float knob_val = *(self->time_step);
    int step_idx = (int)(knob_val * 8.0f + 0.5f);
    if (step_idx < 0) step_idx = 0;
    if (step_idx > 8) step_idx = 8;
    float beat_setting = BEAT_VALUES[step_idx];

    double samples_per_beat = (60.0 / bpm) * self->sample_rate;

    self->target_samples = (uint32_t)(samples_per_beat * beat_setting);
    self->max_8_beat_samples = (uint32_t)(samples_per_beat * MAX_BEATS);

    if (self->target_samples > self->buffer_capacity) self->target_samples = self->buffer_capacity;
    if (self->max_8_beat_samples > self->buffer_capacity) self->max_8_beat_samples = self->buffer_capacity;

    float dt = 1.0f / (float)self->sample_rate;
    float rc = 1.0f / (2.0f * 3.14159265f * 80.0f);
    float alpha = rc / (rc + dt);

    for (uint32_t i = 0; i < sample_count; ++i) {
        float in_left = self->in_l[i];
        float in_right = self->in_r[i];

        // Transiciones basadas en el Activador
        if (self->state == STATE_IDLE && act >= 0.5f) {
            self->state = STATE_RECORDING;
            self->write_ptr = 0;
            self->read_ptr = 0;
            self->total_recorded = 0;
            self->beat_sample_counter = 0; // Sincronizar metrónomo al trigger
        } else if (act < 0.5f) {
            self->state = STATE_IDLE;
        }

        // Sintetizador del Metrónomo (Sonará solo cuando el plugin esté Activo)
        float beep = 0.0f;
        if (self->state != STATE_IDLE) {
            uint32_t beep_duration_samples = (uint32_t)(self->sample_rate * 0.05); // 50ms de duración
            if (self->beat_sample_counter < beep_duration_samples) {
                // Generar onda senoidal pura a 1000Hz para el "click"
                beep = sinf(2.0f * 3.14159265f * 1000.0f * (float)self->beat_sample_counter / (float)self->sample_rate) * 0.15f;
            }

            self->beat_sample_counter++;
            if (self->beat_sample_counter >= (uint32_t)samples_per_beat) {
                self->beat_sample_counter = 0; // Reiniciar contador exactamente en cada beat
            }
        }

        if (self->state == STATE_IDLE) {
            self->out_l[i] = in_left;
            self->out_r[i] = in_right;

        } else if (self->state == STATE_RECORDING) {
            self->buffer_l[self->write_ptr] = in_left;
            self->buffer_r[self->write_ptr] = in_right;
            self->write_ptr++;
            self->total_recorded++;

            // Mezclamos el pitido del metrónomo sobre la salida
            self->out_l[i] = in_left + beep;
            self->out_r[i] = in_right + beep;

            if (self->total_recorded >= self->target_samples) {
                self->state = STATE_LOOPING;
            }

        } else if (self->state == STATE_LOOPING) {
            float loop_l = self->buffer_l[self->read_ptr];
            float loop_r = self->buffer_r[self->read_ptr];

            self->read_ptr++;
            if (self->read_ptr >= self->target_samples) {
                self->read_ptr = 0;
                for (uint32_t k = 0; k < self->target_samples; ++k) {
                    self->buffer_l[k] *= fb;
                    self->buffer_r[k] *= fb;
                }
            }

            if (self->total_recorded < self->max_8_beat_samples) {
                self->buffer_l[self->write_ptr] = in_left;
                self->buffer_r[self->write_ptr] = in_right;
                self->write_ptr++;
                self->total_recorded++;
            }

            if (hpf >= 0.5f) {
                float filtered_l = alpha * (self->hpf_y1_l + loop_l - self->hpf_x1_l);
                self->hpf_x1_l = loop_l;
                self->hpf_y1_l = filtered_l;
                loop_l = filtered_l;

                float filtered_r = alpha * (self->hpf_y1_r + loop_r - self->hpf_x1_r);
                self->hpf_x1_r = loop_r;
                self->hpf_y1_r = filtered_r;
                loop_r = filtered_r;
            }

            float wet_gain = 1.0f;
            float dry_gain = 1.0f;

            if (mix < 0.5f) {
                wet_gain = mix * 2.0f;
                dry_gain = 1.0f;
            } else {
                wet_gain = 1.0f;
                dry_gain = (1.0f - mix) * 2.0f;
            }

            // Mezclar salida e incluir el metrónomo de diagnóstico
            self->out_l[i] = (in_left * dry_gain) + (loop_l * wet_gain) + beep;
            self->out_r[i] = (in_right * dry_gain) + (loop_r * wet_gain) + beep;
        }
    }
}

static void deactivate(LV2_Handle instance) {}

static void cleanup(LV2_Handle instance) {
    HybridLooper* self = (HybridLooper*)instance;
    free(self->buffer_l);
    free(self->buffer_r);
    free(self);
}

static const LV2_Descriptor descriptor = {
    LOOPER_URI, instantiate, connect_port, activate, run, deactivate, cleanup, NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}
