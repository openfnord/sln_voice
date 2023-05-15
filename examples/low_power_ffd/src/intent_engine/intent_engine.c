// Copyright (c) 2022-2023 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public License: Version 1

/* STD headers */
#include <platform.h>
#include <xs1.h>
#include <xcore/hwtimer.h>

/* FreeRTOS headers */
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"

/* App headers */
#include "app_conf.h"
#include "asr.h"
#include "device_memory_impl.h"
#include "gpio_ctrl/leds.h"
#include "intent_engine/intent_engine.h"
#include "platform/driver_instances.h"
#include "power/power_state.h"
#include "power/power_control.h"

#if ON_TILE(ASR_TILE_NO)

// This define is referenced by the model source/header files.
#ifndef ALIGNED
#define ALIGNED(x) __attribute__ ((aligned((x))))
#endif

#define SEARCH_VAR gs_command_grammarLabel

#ifdef COMMAND_SEARCH_HEADER_FILE
#include COMMAND_SEARCH_HEADER_FILE
#endif

#ifdef COMMAND_SEARCH_SOURCE_FILE
#include COMMAND_SEARCH_SOURCE_FILE
#else
extern const unsigned short SEARCH_VAR[];
#endif

#define IS_COMMAND(id)      ((id) > 0)
#define SAMPLES_PER_ASR     (appconfINTENT_SAMPLE_BLOCK_LENGTH)

typedef enum intent_power_state {
    STATE_REQUESTING_LOW_POWER,
    STATE_ENTERING_LOW_POWER,
    STATE_ENTERED_LOW_POWER,
    STATE_EXITING_LOW_POWER,
    STATE_EXITED_LOW_POWER
} intent_power_state_t;

enum timeout_event {
    TIMEOUT_EVENT_NONE = 0,
    TIMEOUT_EVENT_INTENT = 1
};

// Sensory NET model file is in flash at the offset specified in the CMakeLists
// QSPI_FLASH_MODEL_START_ADDRESS variable.  The XS1_SWMEM_BASE value needs
// to be added so the address in in the SwMem range.
uint16_t *dnn_netLabel = (uint16_t *) (XS1_SWMEM_BASE + QSPI_FLASH_MODEL_START_ADDRESS);

static asr_port_t asr_ctx;
static devmem_manager_t devmem_ctx;

static intent_power_state_t intent_power_state;
static uint8_t requested_full_power;

static uint32_t timeout_event = TIMEOUT_EVENT_NONE;

static void vIntentTimerCallback(TimerHandle_t pxTimer);
static void receive_audio_frames(StreamBufferHandle_t input_queue, int32_t *buf,
                                 int16_t *buf_short, size_t *buf_short_index);
static void timeout_event_handler(TimerHandle_t pxTimer);
static void hold_intent_state(TimerHandle_t pxTimer);
static void hold_full_power(TimerHandle_t pxTimer);
static uint8_t low_power_handler(TimerHandle_t pxTimer, int32_t *buf,
                                 int16_t *buf_short, size_t *buf_short_index);
static void wait_for_keyword_queue_completion(void);

static void vIntentTimerCallback(TimerHandle_t pxTimer)
{
    timeout_event |= TIMEOUT_EVENT_INTENT;
}

static void receive_audio_frames(StreamBufferHandle_t input_queue, int32_t *buf,
                                 int16_t *buf_short, size_t *buf_short_index)
{
    uint8_t *buf_ptr = (uint8_t*)buf;
    size_t buf_len = appconfINTENT_SAMPLE_BLOCK_LENGTH * sizeof(int32_t);

    do {
        size_t bytes_rxed = xStreamBufferReceive(input_queue,
                                                 buf_ptr,
                                                 buf_len,
                                                 portMAX_DELAY);
        buf_len -= bytes_rxed;
        buf_ptr += bytes_rxed;
    } while (buf_len > 0);

    for (int i = 0; i < appconfINTENT_SAMPLE_BLOCK_LENGTH; i++) {
        buf_short[(*buf_short_index)++] = buf[i] >> 16;
    }
}

static void timeout_event_handler(TimerHandle_t pxTimer)
{
    if (timeout_event & TIMEOUT_EVENT_INTENT) {
        timeout_event &= ~TIMEOUT_EVENT_INTENT;
        if (intent_engine_low_power_ready()) {
            intent_power_state = STATE_REQUESTING_LOW_POWER;
            power_control_req_low_power();
        } else {
            hold_full_power(pxTimer);
        }
    }
}

static void hold_intent_state(TimerHandle_t pxTimer)
{
    xTimerStop(pxTimer, 0);
    xTimerChangePeriod(pxTimer, pdMS_TO_TICKS(appconfINTENT_RESET_DELAY_MS), 0);
    timeout_event = TIMEOUT_EVENT_NONE;
    xTimerReset(pxTimer, 0);
}

static void wait_for_keyword_queue_completion(void)
{
    const TickType_t poll_interval = pdMS_TO_TICKS(100);

    while (!intent_engine_low_power_ready()) {
        vTaskDelay(poll_interval);
    }
}

static void hold_full_power(TimerHandle_t pxTimer)
{
    xTimerStop(pxTimer, 0);
    xTimerChangePeriod(pxTimer, pdMS_TO_TICKS(appconfLOW_POWER_INHIBIT_MS), 0);
    timeout_event = TIMEOUT_EVENT_NONE;
    xTimerReset(pxTimer, 0);
}

static uint8_t low_power_handler(TimerHandle_t pxTimer, int32_t *buf,
                                 int16_t *buf_short, size_t *buf_short_index)
{
    uint8_t low_power = 0;

    switch (intent_power_state) {
    case STATE_REQUESTING_LOW_POWER:
        low_power = 1;
        // Wait here until other tile accepts/rejects the request.
        if (requested_full_power) {
            requested_full_power = 0;
            // Aborting low power transition.
            intent_power_state = STATE_EXITING_LOW_POWER;
        }
        break;
    case STATE_ENTERING_LOW_POWER:
        /* Prior to entering this state, the other tile is to cease pushing
         * samples to the stream buffer. */
        memset(buf, 0, appconfINTENT_SAMPLE_BLOCK_LENGTH);
        memset(buf_short, 0, SAMPLES_PER_ASR);
        *buf_short_index = 0;
        intent_engine_stream_buf_reset();
        wait_for_keyword_queue_completion();
        intent_power_state = STATE_ENTERED_LOW_POWER;
        break;
    case STATE_ENTERED_LOW_POWER:
        low_power = 1;
        if (requested_full_power) {
            requested_full_power = 0;
            intent_power_state = STATE_EXITING_LOW_POWER;
        }
        break;
    case STATE_EXITING_LOW_POWER:
        asr_reset(asr_ctx);
        hold_intent_state(pxTimer);
        led_indicate_idle();
        intent_power_state = STATE_EXITED_LOW_POWER;
        break;
    case STATE_EXITED_LOW_POWER:
    default:
        break;
    }

    return low_power;
}

void intent_engine_full_power_request(void)
{
    requested_full_power = 1;
}

void intent_engine_low_power_accept(void)
{
    // The request has been accepted proceed with finalizing low power transition.
    intent_power_state = STATE_ENTERING_LOW_POWER;
}

#pragma stackfunction 1500
void intent_engine_task(void *args)
{
    StreamBufferHandle_t input_queue = (StreamBufferHandle_t)args;
    int32_t buf[appconfINTENT_SAMPLE_BLOCK_LENGTH] = {0};
    int16_t buf_short[SAMPLES_PER_ASR] = {0};
    size_t buf_short_index = 0;
    asr_error_t asr_error = ASR_OK;
    asr_result_t asr_result;

    TimerHandle_t int_eng_tmr = xTimerCreate(
        "int_eng_tmr",
        pdMS_TO_TICKS(appconfINTENT_RESET_DELAY_MS),
        pdFALSE,
        NULL,
        vIntentTimerCallback);

    devmem_init(&devmem_ctx);
    asr_ctx = asr_init((void *)dnn_netLabel, (void *)SEARCH_VAR, &devmem_ctx);

    /* Immediately signal intent timeout, to start a request to enter low power.
     * This is to help prevent commands from being detected at startup
     * (without a wake-word event). */
    timeout_event |= TIMEOUT_EVENT_INTENT;
    requested_full_power = 0;

    /* Alert other tile to start the audio pipeline */
    int dummy = 0;
    rtos_intertile_tx(intertile_ctx, appconfINTENT_ENGINE_READY_SYNC_PORT, &dummy, sizeof(dummy));

    while (1)
    {
        timeout_event_handler(int_eng_tmr);

        if (low_power_handler(int_eng_tmr, buf, buf_short, &buf_short_index)) {
            // Low power, processing stopped.
            continue;
        }

        receive_audio_frames(input_queue, buf, buf_short, &buf_short_index);

        if (buf_short_index < SAMPLES_PER_ASR)
            continue;

        buf_short_index = 0; // reset the offset into the buffer of int16s.
                             // Note, we do not need to overlap the window of samples.
                             // This is handled in the ASR ports.

        asr_error = asr_process(asr_ctx, buf_short, SAMPLES_PER_ASR);

        if (asr_error == ASR_OK) {
            asr_error = asr_get_result(asr_ctx, &asr_result);
        }

        if (asr_error != ASR_OK) {
            debug_printf("ASR error on tile %d: %d\n", THIS_XCORE_TILE, asr_error);
            continue;
        }

        if (IS_COMMAND(asr_result.id)) {
            hold_intent_state(int_eng_tmr);
            intent_engine_process_asr_result(asr_result.id);
        }
    }
}

#endif /* ON_TILE(ASR_TILE_NO) */
