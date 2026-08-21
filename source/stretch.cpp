#include "stretch.h"

#include <SoundTouch.h>

static soundtouch::SoundTouch *processor;
static uint32_t configured_rate;

bool stretch_init(uint32_t sample_rate) {
    if (!processor) processor = new soundtouch::SoundTouch();
    if (!processor) return false;
    configured_rate = sample_rate;
    processor->clear();
    processor->setSampleRate(sample_rate);
    processor->setChannels(2);
    processor->setSetting(SETTING_USE_AA_FILTER, 0);
    processor->setSetting(SETTING_USE_QUICKSEEK, 1);
    processor->setTempo(1.0f);
    return true;
}

void stretch_reset(uint32_t sample_rate, float tempo_factor) {
    if (!stretch_init(sample_rate)) return;
    processor->clear();
    processor->setTempo(tempo_factor < 0.25f ? 0.25f : tempo_factor > 2.0f ? 2.0f : tempo_factor);
}

void stretch_set_tempo(float tempo_factor) {
    if (!processor) return;
    processor->setTempo(tempo_factor < 0.25f ? 0.25f : tempo_factor > 2.0f ? 2.0f : tempo_factor);
}

void stretch_put(const int16_t *samples, uint32_t frames) {
    if (processor && samples && frames)
        processor->putSamples((const soundtouch::SAMPLETYPE *)samples, frames);
}

uint32_t stretch_receive(int16_t *samples, uint32_t max_frames) {
    if (!processor || !samples || !max_frames) return 0;
    return processor->receiveSamples((soundtouch::SAMPLETYPE *)samples, max_frames);
}

void stretch_exit(void) {
    delete processor;
    processor = 0;
    configured_rate = 0;
}
