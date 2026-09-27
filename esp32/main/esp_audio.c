// SOTN ESP32-S3 — F6: software SPU pull.
//
// The psyz SPU mixer is platform-agnostic; a backend just pulls 44100 Hz
// stereo s16 frames from Psyz_SpuPullSamples, which also advances the RCnt
// sequencer clock in SAMPLE units (the PC SDL callback works the same way).
// This board has no DAC/speaker, so the PCM is mixed and dropped — the pull
// exists for game-clock fidelity and to prove the voice mixer works; wiring
// I2S/PDM out later only needs to consume `pcm` instead of discarding it.
//
// 441 frames per batch = exactly 10 ms at 44100 Hz = 10 FreeRTOS ticks
// (CONFIG_FREERTOS_HZ=1000), so vTaskDelayUntil paces drift-free.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <psyz/spu.h>

#define BATCH_FRAMES 441
#define BATCH_PERIOD_TICKS pdMS_TO_TICKS(10)

static short pcm[BATCH_FRAMES * 2];
static SemaphoreHandle_t sAudioMutex;

// telemetry read by esp_shim's frame line
unsigned sotn_audio_batches;
unsigned sotn_audio_active; // batches containing any nonzero sample

void Psyz_AudioLock(void) {
    if (sAudioMutex) {
        xSemaphoreTakeRecursive(sAudioMutex, portMAX_DELAY);
    }
}

void Psyz_AudioUnlock(void) {
    if (sAudioMutex) {
        xSemaphoreGiveRecursive(sAudioMutex);
    }
}

static void audioTask(void* arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, BATCH_PERIOD_TICKS);
        Psyz_AudioLock();
        Psyz_SpuPullSamples(pcm, BATCH_FRAMES); // ticks RCnt internally
        Psyz_AudioUnlock();
        sotn_audio_batches++;
        for (int i = 0; i < BATCH_FRAMES * 2; i++) {
            if (pcm[i]) {
                sotn_audio_active++;
                break;
            }
        }
    }
}

int Psyz_AudioInit(void) {
    if (sAudioMutex) {
        return 0;
    }
    Psyz_SpuInit();
    sAudioMutex = xSemaphoreCreateRecursiveMutex();
    xTaskCreatePinnedToCore(audioTask, "spu_pull", 4096, NULL, 6, NULL, 1);
    return 0;
}
