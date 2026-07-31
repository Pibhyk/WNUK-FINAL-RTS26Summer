/*
 * Capstone — Industrial Safety Work-Cell (merged App 4 + App 5)
 *
 * Theme: INDUSTRIAL — button input -> state-machine update -> operator
 * display, plus a priority-inversion demo (mutex vs. binary semaphore)
 * running alongside it — four IPC primitives total in one build.
 *
 * Scaffold Code - AI useage:
 *   Addition of the USE_WEBSERVER compile-time switch and a working serial
 *     monitor task on Core 0, plus per-task heartbeat counters
 *   Logic to allow for switching between a serial monitor and the (student-built)
 *     web monitor, so the pipeline runs in Wokwi with no Wi-Fi by default
 *   Commenting of code including human readable summaries
 *
 * Student additions (this pass):
 *   - Themed data item: a panel-input event (timestamp + input code).
 *   - input_task (was producer_task): polls a simulated control-panel input,
 *     pushes an event into data_q with a back-pressure policy (drop oldest).
 *   - statemachine_task (was consumer_task): pops an event, advances a
 *     3-state machine (IDLE / RUNNING / FAULT), logs the transition.
 *   - coordinator_task: unchanged plumbing, notifies display_task.
 *   - display_task (was responder_task): renders the "operator display" line;
 *     also woken directly by the panel button ISR for a manual refresh.
 *   - USE_WEBSERVER left at 0 (serial/terminal monitor) per current run —
 *     web monitor stub is left in place but not implemented this pass.
 *
 * ============================================================
 *  RUN MODE  (serial monitor vs. web monitor)
 * ============================================================
 *
 * USE_WEBSERVER selects the Core-0 observability plane. The Core-1 pipeline is
 * identical in both modes.
 *
 *   USE_WEBSERVER = 0  -> Serial monitor (provided, working). Prints queue depth,
 *                         event bits, and heartbeats once a second. No Wi-Fi, so
 *                         the pipeline runs in Wokwi out of the box. THIS RUN.
 *   USE_WEBSERVER = 1  -> Web monitor stub — not implemented this pass.
 *
 * ============================================================
 * Theme: INDUSTRIAL — panel input -> state machine -> operator display
 * ============================================================
 */

#ifndef USE_WEBSERVER
#define USE_WEBSERVER 0
#endif

/* Merged in from App 4: selects the lock type for the priority-inversion
 * demo below (pi_lock). Both modes run the same H/M/L scenario; only the
 * lock primitive differs. See the demo section further down for details. */
#ifndef USE_PI_MUTEX
#define USE_PI_MUTEX 1
#endif

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#endif

#define BUTTON_GPIO GPIO_NUM_18

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "app5";

/* ---------- Themed data item ----------
 * One panel-input event: when it happened, and what the operator did.
 * INPUT_NONE / INPUT_START / INPUT_STOP / INPUT_FAULT drive the state
 * machine in statemachine_task. */
typedef enum {
    INPUT_NONE        = 0,
    INPUT_START       = 1,
    INPUT_STOP        = 2,
    INPUT_FAULT       = 3,
    INPUT_CLEAR_FAULT = 4,   /* only input accepted while in FAULT */
} panel_input_t;

typedef struct {
    uint32_t timestamp_ms;
    int      value;   /* panel_input_t */
} panel_event_t;

/* Machine states driven by statemachine_task */
typedef enum { MSTATE_IDLE = 0, MSTATE_RUNNING = 1, MSTATE_FAULT = 2 } machine_state_t;
static const char *state_name(machine_state_t s)
{
    switch (s) {
        case MSTATE_IDLE:    return "IDLE";
        case MSTATE_RUNNING: return "RUNNING";
        case MSTATE_FAULT:   return "FAULT";
        default:             return "UNKNOWN";
    }
}
static volatile machine_state_t current_state = MSTATE_IDLE;

/* ---------- IPC objects (created in app_main, used everywhere) ---------- */
/* Queue depth 8, item size = sizeof(panel_event_t).
 * Producer runs at 20 Hz (one event per 50 ms). Consumer does a small amount
 * of themed work per item (state transition + log), which is fast, so under
 * normal load the queue rarely holds more than 1-2 items. Depth 8 gives
 * ~400 ms of absorption (8 * 50 ms) for a burst where the consumer is
 * briefly delayed by a higher-priority task (coordinator/responder), without
 * growing so large that a real backlog goes unnoticed for a long time. */
static QueueHandle_t      data_q;
static EventGroupHandle_t evt_group;
static TaskHandle_t       responder_handle;   /* display_task's handle */

/* Event-group bit definitions */
#define EV_BIT_DATA_PRODUCED  (1 << 0)
#define EV_BIT_DATA_PROCESSED (1 << 1)

/* Per-task heartbeats — proof of life for the monitor. Single 32-bit reads are
 * atomic on Xtensa, so the monitor can read these without a lock (App 6's topic). */
static volatile uint32_t hb_prod, hb_cons, hb_coord, hb_resp;

/* ---------- Input task (Core 1) — was producer_task ----------
 * Polls a simulated control-panel input and pushes a themed event into
 * data_q. Back-pressure policy: try a short-timeout send; if the queue is
 * still full (consumer badly behind), drop the OLDEST queued event to make
 * room, since the newest panel input is what the operator cares about. */
static void input_task(void *arg)
{
    int tick = 0;
    for (;;) {
        panel_event_t evt;
        evt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* Simulated panel activity: mostly idle polling, occasionally a
         * START, occasionally a FAULT, to exercise the state machine. */
        if (tick % 40 == 5)       evt.value = INPUT_START;
        else if (tick % 40 == 25) evt.value = INPUT_STOP;
        else if (tick % 97 == 0)  evt.value = INPUT_FAULT;
        else if (tick % 97 == 40) evt.value = INPUT_CLEAR_FAULT;  /* operator clears fault */
        else                      evt.value = INPUT_NONE;

        if (xQueueSend(data_q, &evt, pdMS_TO_TICKS(10)) != pdTRUE) {
            /* Queue still full after the timeout: drop the oldest event to
             * make room for this one, rather than silently discarding the
             * newest panel input. */
            panel_event_t discard;
            xQueueReceive(data_q, &discard, 0);
            xQueueSend(data_q, &evt, 0);
            ESP_LOGW(TAG, "[input] queue full — dropped oldest event to admit newest");
        }

        xEventGroupSetBits(evt_group, EV_BIT_DATA_PRODUCED);

        tick++;
        hb_prod++;
        vTaskDelay(pdMS_TO_TICKS(50));   /* 20 Hz producer */
    }
}

/* ---------- State-machine task (Core 1) — was consumer_task ----------
 * Pops a panel event, advances the 3-state machine, logs any transition. */
static void statemachine_task(void *arg)
{
    panel_event_t evt;
    for (;;) {
        if (xQueueReceive(data_q, &evt, pdMS_TO_TICKS(50)) == pdTRUE) {
            machine_state_t before = current_state;
            switch (evt.value) {
                case INPUT_START:
                    if (current_state != MSTATE_FAULT) current_state = MSTATE_RUNNING;
                    break;
                case INPUT_STOP:
                    if (current_state != MSTATE_FAULT) current_state = MSTATE_IDLE;
                    break;
                case INPUT_FAULT:
                    current_state = MSTATE_FAULT;
                    break;
                case INPUT_CLEAR_FAULT:
                    if (current_state == MSTATE_FAULT) current_state = MSTATE_IDLE;
                    break;
                case INPUT_NONE:
                default:
                    break;   /* no transition */
            }
            if (current_state != before) {
                ESP_LOGI(TAG, "[state] %s -> %s (input=%d @ %lu ms)",
                         state_name(before), state_name(current_state),
                         evt.value, (unsigned long)evt.timestamp_ms);
            }
        }
        /* Whether or not an item arrived this period, mark this cycle
         * "processed" so the coordinator's rendezvous can proceed. */
        xEventGroupSetBits(evt_group, EV_BIT_DATA_PROCESSED);
        hb_cons++;
    }
}

/* ---------- Coordinator task (Core 1) ----------
 * Waits for BOTH event bits to be set, then signals the responder via direct
 * task notification. Unchanged plumbing from the scaffold.
 */
static void coordinator_task(void *arg)
{
    const EventBits_t wait_mask = EV_BIT_DATA_PRODUCED | EV_BIT_DATA_PROCESSED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(evt_group, wait_mask,
                                              pdTRUE,   /* clear on exit */
                                              pdTRUE,   /* wait for ALL */
                                              portMAX_DELAY);
        if ((got & wait_mask) == wait_mask) {
            /* One full input->state-update cycle has completed; tell the
             * display to refresh. */
            xTaskNotifyGive(responder_handle);
            hb_coord++;
        }
    }
}

/* ---------- Display task (Core 1) — was responder_task ----------
 * Wakes via direct task notification from coordinator OR from the panel
 * button ISR (manual refresh / operator acknowledge).
 */
static void display_task(void *arg)
{
    machine_state_t last_shown = (machine_state_t)-1;   /* force first log */
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n == 0) continue;
        machine_state_t now = current_state;
        if (now != last_shown) {
            ESP_LOGI(TAG, "[display] operator display: state=%s (notify count=%lu)",
                     state_name(now), (unsigned long)n);
            last_shown = now;
        }
        hb_resp++;
    }
}

/* ---------- Button ISR — notify display task directly ----------
 * Models an operator "acknowledge / refresh display now" button, independent
 * of the normal input->state-machine->coordinator path. */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 200) return;
    last_edge_us = now;

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(responder_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

#if USE_WEBSERVER
/* ---------- Web monitor task (Core 0)  [USE_WEBSERVER = 1] ----------
 * Not implemented this pass — running with USE_WEBSERVER=0 (terminal
 * monitor) instead. Left as a stub for a future pass. */
static void webmonitor_task(void *arg)
{
    ESP_LOGI(TAG, "[webmon] stub — not implemented this pass (using terminal monitor)");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
#else
/* ---------- Serial monitor task (Core 0)  [USE_WEBSERVER = 0] ----------
 * Provided and working. Prints queue depth, event bits, heartbeats, and the
 * current machine state once a second. This is the monitor used for this
 * assignment (web monitor deliverable deferred). */
static void serial_monitor_task(void *arg)
{
    for (;;) {
        UBaseType_t depth = uxQueueMessagesWaiting(data_q);
        EventBits_t bits  = xEventGroupGetBits(evt_group);
        ESP_LOGI(TAG,
                 "[monitor] q_depth=%u  evt=0x%02x  state=%s  hb: prod=%lu cons=%lu coord=%lu resp=%lu",
                 (unsigned)depth, (unsigned)bits, state_name(current_state),
                 (unsigned long)hb_prod, (unsigned long)hb_cons,
                 (unsigned long)hb_coord, (unsigned long)hb_resp);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif /* USE_WEBSERVER */

/* ============================================================
 *  Priority-inversion demo (merged in from App 4) — tasks H / M / L
 * ============================================================
 * Independent of the pipeline above; demonstrates a fourth primitive
 * (mutex, or a binary semaphore used as a lock) under real contention.
 *
 *   USE_PI_MUTEX = 1 -> H and L share a FreeRTOS MUTEX (priority
 *                       inheritance ON). H's wait is bounded by L's
 *                       remaining critical section.
 *   USE_PI_MUTEX = 0 -> H and L share a BINARY SEMAPHORE used as a lock
 *                       (no ownership, no inheritance). M can preempt L
 *                       while L holds the lock, so H waits for M too —
 *                       classic unbounded priority inversion.
 *
 * Runs pinned to Core 1 alongside the pipeline tasks above. L/M/H sit at
 * priorities 5/10/15, distinct from the pipeline's 8/9/12, so the two
 * demos coexist without one starving the other under normal load.
 */
#if USE_PI_MUTEX
#define PI_LOCK_CREATE() xSemaphoreCreateMutex()
#define PI_LOCK_NAME     "MUTEX (priority inheritance ON)"
#else
#define PI_LOCK_CREATE() xSemaphoreCreateBinary()
#define PI_LOCK_NAME     "BINARY SEM (no inheritance)"
#endif

static SemaphoreHandle_t pi_lock;

#define PI_H_DELAY_MS  50
#define PI_M_DELAY_MS  100

/* Tuned for ~1.63M iterations/sec (measured on Wokwi target) so L~500ms,
 * M~1000ms when each runs alone. Re-measure and rescale for other targets. */
#define PI_L_ITERS  800000UL
#define PI_M_ITERS  1600000UL

static volatile uint32_t pi_sink;
static void pi_burn(uint32_t iters)
{
    uint32_t x = pi_sink ? pi_sink : 1u;
    for (uint32_t i = 0; i < iters; i++) { x ^= (x << 5); x += i; }
    pi_sink = x;
}

static void pi_low_task(void *arg)
{
    xSemaphoreTake(pi_lock, portMAX_DELAY);
    int64_t t_acq = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][L] took lock @ %lld us — entering CPU-bound section",
             (long long)t_acq);
    pi_burn(PI_L_ITERS);
    int64_t t_rel = esp_timer_get_time();
    xSemaphoreGive(pi_lock);
    ESP_LOGI(TAG, "[PI][L] released lock @ %lld us (held %lld us wall-clock)",
             (long long)t_rel, (long long)(t_rel - t_acq));
    vTaskDelete(NULL);
}

static void pi_med_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(PI_M_DELAY_MS));
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][M] ready @ %lld us — burning CPU (takes no lock)",
             (long long)t0);
    pi_burn(PI_M_ITERS);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][M] done  @ %lld us (ran %lld us wall-clock)",
             (long long)t1, (long long)(t1 - t0));
    vTaskDelete(NULL);
}

static void pi_high_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(PI_H_DELAY_MS));
    int64_t t_block = esp_timer_get_time();
    ESP_LOGI(TAG, "[PI][H] wants lock @ %lld us — blocking", (long long)t_block);
    xSemaphoreTake(pi_lock, portMAX_DELAY);
    int64_t t_acq = esp_timer_get_time();
    int64_t wait = t_acq - t_block;
    ESP_LOGW(TAG, "[PI][H] ACQUIRED @ %lld us — waited %lld us (~%lld ms)  [lock=%s]",
             (long long)t_acq, (long long)wait, (long long)(wait / 1000), PI_LOCK_NAME);
    xSemaphoreGive(pi_lock);
    vTaskDelete(NULL);
}

static void start_inversion_demo(void)
{
    pi_lock = PI_LOCK_CREATE();
#if !USE_PI_MUTEX
    xSemaphoreGive(pi_lock);
#endif
    ESP_LOGI(TAG, "[PI] inversion demo lock = %s", PI_LOCK_NAME);
    xTaskCreatePinnedToCore(pi_high_task, "H", 4096, NULL, 15, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(pi_med_task,  "M", 4096, NULL, 10, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(pi_low_task,  "L", 4096, NULL,  5, NULL, APP_CPU_NUM);
}

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== App 5 [INDUSTRIAL] starting — IPC pipeline ====");

#if USE_WEBSERVER
    ESP_LOGI(TAG, "Monitor: WEB (USE_WEBSERVER=1) — webmonitor_task stub (Core 0)");
#else
    ESP_LOGI(TAG, "Monitor: SERIAL/TERMINAL (USE_WEBSERVER=0) — Core-0 summary once/sec, no Wi-Fi");
#endif
    ESP_LOGI(TAG, "PI demo lock: %s (USE_PI_MUTEX=%d)", PI_LOCK_NAME, USE_PI_MUTEX);

    /* Depth 8, item = panel_event_t. See sizing rationale above data_q. */
    data_q = xQueueCreate(/*depth=*/ 8, /*item size=*/ sizeof(panel_event_t));

    evt_group = xEventGroupCreate();

    /* Tasks on Core 1 (real-time plane). 4096-byte stacks: any task that calls
     * ESP_LOGI needs headroom for the vprintf formatting (2048 overflows). */
    xTaskCreatePinnedToCore(input_task,        "input",  4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(statemachine_task, "state",  4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(coordinator_task,  "coord",  4096, NULL,  9, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(display_task,      "display",4096, NULL, 12, &responder_handle, APP_CPU_NUM);

    /* Observability plane on Core 0 (networking plane) */
#if USE_WEBSERVER
    xTaskCreatePinnedToCore(webmonitor_task,    "webmon",  4096, NULL, 4, NULL, PRO_CPU_NUM);
#else
    xTaskCreatePinnedToCore(serial_monitor_task, "monitor", 4096, NULL, 4, NULL, PRO_CPU_NUM);
#endif

    /* Button ISR — operator "acknowledge / refresh" button */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

    /* Priority-inversion demo (merged from App 4). Runs alongside the
     * pipeline; flip USE_PI_MUTEX to compare bounded vs. unbounded wait. */
    start_inversion_demo();
}