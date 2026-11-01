#include "audio_capture.h"

#include <stdbool.h>
#include <string.h>

#include "i2s_rx.h"
#include "audio_device.h"
#include "pico/stdlib.h"

#ifndef PICODAC_I2S_RX_DATA_PIN
#define PICODAC_I2S_RX_DATA_PIN 23
#endif

#define CAPTURE_RING_FRAMES 2048u
#define CAPTURE_PREROLL_MS 4u
#define CAPTURE_MAX_PACKET_FRAMES 49u
#define CAPTURE_BYTES_PER_FRAME 8u
#define CAPTURE_ACCUMULATOR_LIMIT 1000000LL

static int32_t capture_ring[CAPTURE_RING_FRAMES * 2u];
static uint32_t capture_read_frame;
static uint32_t capture_write_frame;
static uint32_t capture_fill_frames;
static int64_t capture_rate_accumulator;
static uint32_t capture_packet_phase;
static uint32_t capture_sample_rate = 48000u;
static bool capture_running;
static bool capture_primed;

static void capture_ring_clear(void) {
    capture_read_frame = 0;
    capture_write_frame = 0;
    capture_fill_frames = 0;
    capture_rate_accumulator = 0;
    capture_packet_phase = 0;
    capture_primed = false;
}

static void capture_ring_push(const int32_t *samples, uint32_t frames) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (capture_fill_frames == CAPTURE_RING_FRAMES) {
            /* Keep the newest audio if the USB host stops consuming packets. */
            capture_read_frame = (capture_read_frame + 1u) % CAPTURE_RING_FRAMES;
            --capture_fill_frames;
        }

        const uint32_t destination = capture_write_frame * 2u;
        capture_ring[destination] = samples[frame * 2u];
        capture_ring[destination + 1u] = samples[frame * 2u + 1u];
        capture_write_frame = (capture_write_frame + 1u) % CAPTURE_RING_FRAMES;
        ++capture_fill_frames;
    }
}

static bool capture_ring_pop_frame(int32_t *left, int32_t *right) {
    if (capture_fill_frames == 0) {
        return false;
    }

    const uint32_t source = capture_read_frame * 2u;
    *left = capture_ring[source];
    *right = capture_ring[source + 1u];
    capture_read_frame = (capture_read_frame + 1u) % CAPTURE_RING_FRAMES;
    --capture_fill_frames;
    return true;
}

void audio_capture_init(void) {
    capture_ring_clear();
    capture_running = false;

    if (!i2s_rx_init(PICODAC_I2S_RX_DATA_PIN)) {
        panic("I2S RX initialization failed");
    }
}

void audio_capture_stream_start(void) {
    capture_ring_clear();
    capture_sample_rate = audio_device_get_sampling_freq();
    capture_running = true;
    i2s_rx_start();
}

void audio_capture_stream_stop(void) {
    capture_running = false;
    i2s_rx_stop();
    capture_ring_clear();
}

void audio_capture_task(void) {
    if (!capture_running) {
        return;
    }

    const int32_t *completed_samples;
    while (i2s_rx_take_buffer(&completed_samples)) {
        capture_ring_push(completed_samples, I2S_RX_BUFFER_FRAMES);
    }
}

uint16_t audio_capture_read_usb_packet(uint8_t *destination, uint16_t capacity) {
    if (!destination || capacity < CAPTURE_MAX_PACKET_FRAMES * CAPTURE_BYTES_PER_FRAME) {
        return 0;
    }

    int32_t *output = (int32_t *)destination;
    uint32_t packet_frames = capture_sample_rate / 1000u;
    capture_packet_phase += capture_sample_rate % 1000u;
    if (capture_packet_phase >= 1000u) {
        ++packet_frames;
        capture_packet_phase -= 1000u;
    }

    if (!capture_running) {
        memset(destination, 0, packet_frames * CAPTURE_BYTES_PER_FRAME);
        return (uint16_t)(packet_frames * CAPTURE_BYTES_PER_FRAME);
    }

    const uint32_t target_frames =
        (capture_sample_rate * CAPTURE_PREROLL_MS) / 1000u;
    if (!capture_primed && capture_fill_frames >= target_frames) {
        capture_primed = true;
    }

    if (!capture_primed) {
        memset(destination, 0, packet_frames * CAPTURE_BYTES_PER_FRAME);
        return (uint16_t)(packet_frames * CAPTURE_BYTES_PER_FRAME);
    }

    /* Adjust packet sizes very slowly to keep the I2S-to-USB FIFO near its
       four-millisecond target. The nominal packet rate follows the selected
       UAC2 clock rate, including fractional 44.1/88.2 kHz frame counts. */
    const int32_t fill_error =
        (int32_t)capture_fill_frames - (int32_t)target_frames;
    int32_t correction_ppm = fill_error * 8;
    if (correction_ppm > 2000) correction_ppm = 2000;
    if (correction_ppm < -2000) correction_ppm = -2000;

    capture_rate_accumulator +=
        (int64_t)correction_ppm * (int64_t)(capture_sample_rate / 1000u + 1u);
    if (capture_rate_accumulator >= CAPTURE_ACCUMULATOR_LIMIT) {
        ++packet_frames;
        capture_rate_accumulator -= CAPTURE_ACCUMULATOR_LIMIT;
    } else if (capture_rate_accumulator <= -CAPTURE_ACCUMULATOR_LIMIT &&
               packet_frames > 1u) {
        --packet_frames;
        capture_rate_accumulator += CAPTURE_ACCUMULATOR_LIMIT;
    }

    if (packet_frames > CAPTURE_MAX_PACKET_FRAMES) {
        packet_frames = CAPTURE_MAX_PACKET_FRAMES;
    }

    for (uint32_t frame = 0; frame < packet_frames; ++frame) {
        int32_t left = 0;
        int32_t right = 0;
        (void)capture_ring_pop_frame(&left, &right);
        output[frame * 2u] = left;
        output[frame * 2u + 1u] = right;
    }

    return (uint16_t)(packet_frames * CAPTURE_BYTES_PER_FRAME);
}
