/*
 * EEL 4775 — Final Integration Capstone
 * Project: SENTINEL — a fail-safe cardiac telemetry monitor
 *
 * One sentence:
 *   A hard-real-time cardiac telemetry monitor that samples an ECG, detects
 *   arrhythmias within a bounded deadline, and fails safe — raising a
 *   persistent alarm the moment its own sensor-freshness deadline is violated.
 *
 * Integration (folds in every prior app):
 *   App 1  web/serial observability sidecar on Core 0
 *   App 2  periodic multi-task scheduler with measured WCET
 *   App 3  notify-vs-semaphore wake-latency benchmark
 *   App 4  synchronization primitives (event group rendezvous)
 *   App 5  dual-core IPC pipeline (queue + event group + task notification)  <-- spine
 *   NEW    fail-safe graceful degradation: a freshness watchdog that detects a
 *          lost sensor and drops the system into a safe ALARM state.
 *
 *   Core 1 (real-time plane): ECG sampler -> arrhythmia detector
 *                             -> coordinator (rendezvous) -> alarm dispatch
 *                             + freshness watchdog (deadline monitor)
 *   Core 0 (observability):   serial monitor (default) OR web monitor
 *
 * Tailored for: embedded firmware engineer, safety-critical / medical devices.
 *
 * Reuse (honor code): Wi-Fi/HTTP follows the App 1 pattern; the latency bench
 * follows the App 3 helper; the IPC spine is App 5. Cited in README.
 */

#ifndef USE_WEBSERVER
#define USE_WEBSERVER 0        /* 0 = serial monitor (default), 1 = web monitor */
#endif

#ifndef RUN_LATENCY_BENCH
#define RUN_LATENCY_BENCH 0    /* 1 = run notify-vs-semaphore wake benchmark at boot */
#endif

/* Freshness deadline: if the detector sees no new sample within this window,
 * the watchdog declares the sensor lost and the system fails safe. */
#define FRESHNESS_DEADLINE_MS 300

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#define LOG_LOCAL_LEVEL 5      /* enable ESP_LOGx before esp_log.h is included */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL      5

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#endif

#define BUTTON_GPIO GPIO_NUM_18

static const char *TAG = "sentinel";

/* ---------- IPC objects ---------- */
static QueueHandle_t      data_q;
static EventGroupHandle_t evt_group;
static TaskHandle_t       responder_handle;

#define EV_BIT_DATA_PRODUCED  (1 << 0)
#define EV_BIT_DATA_PROCESSED (1 << 1)

/* ---------- Themed data item ---------- */
typedef struct {
    uint32_t timestamp_ms;   /* capture time (ms since boot)      */
    int16_t  mv_x100;        /* ECG amplitude in millivolts * 100 */
    uint16_t seq;            /* monotonic sample sequence number  */
} ecg_sample_t;

/* ---------- System state (graceful degradation) ---------- */
typedef enum { STATE_NORMAL = 0, STATE_DEGRADED = 1 } sys_state_t;
static volatile sys_state_t sys_state    = STATE_NORMAL;
static volatile bool        sensor_fault = false;   /* toggled by the button ISR */
static volatile uint32_t    last_sample_ms;         /* last fresh sample seen     */
static volatile uint32_t    degrade_events;         /* # times we failed safe     */

/* ---------- Shared telemetry (single 32-bit reads are atomic on Xtensa) ---------- */
static volatile uint32_t hb_prod, hb_cons, hb_coord, hb_resp, hb_wdog;
static volatile uint32_t dropped_samples;
static volatile uint32_t arrhythmia_count;
static volatile uint32_t last_arrhythmia_seq;
static volatile uint32_t alarms_dispatched;

/* ---------- Measured WCET, microseconds (App 2 style evidence) ---------- */
static volatile uint32_t wcet_prod_us, wcet_cons_us, wcet_coord_us, wcet_resp_us;
static inline void wcet_update(volatile uint32_t *slot, int64_t t0)
{
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    if (dt > *slot) *slot = dt;
}

/* ---------- Deterministic ECG synth (no header deps) ---------- */
static uint32_t rng_state = 0xC0FFEEu;
static inline int16_t baseline_noise(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (int16_t)((int)((rng_state >> 16) % 9) - 4);   /* +/- 0.04 mV */
}
static int16_t synth_ecg_sample(uint16_t seq)
{
    static uint16_t next_beat = 20;
    static uint16_t beat_no   = 0;
    if (seq == next_beat) {
        beat_no++;
        uint16_t interval = (beat_no % 11 == 0) ? 5 : 20;  /* premature every 11th */
        next_beat = (uint16_t)(seq + interval);
        return 120;                                        /* R peak ~1.2 mV */
    }
    return baseline_noise();
}

/* ---------- Producer: ECG sampler (Core 1) ---------- */
static void producer_task(void *arg)
{
    uint16_t seq = 0;
    for (;;) {
        int64_t t0 = esp_timer_get_time();

        /* Fault injection: a "disconnected sensor" produces no data. The
         * sampler keeps its period but enqueues nothing, starving the
         * detector so the freshness watchdog can catch the fault. */
        if (!sensor_fault) {
            ecg_sample_t s = {
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
                .mv_x100      = synth_ecg_sample(seq),
                .seq          = seq,
            };
            /* Non-blocking; on full, drop OLDEST so the freshest sample wins. */
            if (xQueueSend(data_q, &s, 0) == errQUEUE_FULL) {
                ecg_sample_t stale;
                if (xQueueReceive(data_q, &stale, 0) == pdTRUE) {
                    dropped_samples++;
                    xQueueSend(data_q, &s, 0);
                }
            }
            xEventGroupSetBits(evt_group, EV_BIT_DATA_PRODUCED);
            seq++;
        }

        wcet_update(&wcet_prod_us, t0);
        hb_prod++;
        vTaskDelay(pdMS_TO_TICKS(50));   /* 20 Hz sampler */
    }
}

/* ---------- Consumer: arrhythmia detector (Core 1) ---------- */
static void consumer_task(void *arg)
{
    ecg_sample_t s;
    uint32_t last_r_ms = 0;
    for (;;) {
        if (xQueueReceive(data_q, &s, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;  /* no data: the watchdog handles the freshness deadline */
        }
        int64_t t0 = esp_timer_get_time();

        last_sample_ms = s.timestamp_ms;   /* feed the freshness watchdog */

        /* R-peak detect + RR-interval sanity check (300-1500 ms = ~40-200 bpm). */
        if (s.mv_x100 > 100) {
            uint32_t now = s.timestamp_ms;
            if (last_r_ms != 0) {
                uint32_t rr = now - last_r_ms;
                if (rr < 300 || rr > 1500) {
                    arrhythmia_count++;
                    last_arrhythmia_seq = s.seq;
                    ESP_LOGW(TAG, "[detect] arrhythmia seq=%u  RR=%lums out of band",
                             s.seq, (unsigned long)rr);
                }
            }
            last_r_ms = now;
        }

        xEventGroupSetBits(evt_group, EV_BIT_DATA_PROCESSED);
        wcet_update(&wcet_cons_us, t0);
        hb_cons++;
    }
}

/* ---------- Coordinator (Core 1): event-group rendezvous ---------- */
static void coordinator_task(void *arg)
{
    const EventBits_t wait_mask = EV_BIT_DATA_PRODUCED | EV_BIT_DATA_PROCESSED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(evt_group, wait_mask,
                                              pdTRUE, pdTRUE, portMAX_DELAY);
        if ((got & wait_mask) == wait_mask) {
            int64_t t0 = esp_timer_get_time();
            if (responder_handle != NULL) {
                xTaskNotifyGive(responder_handle);
            }
            wcet_update(&wcet_coord_us, t0);
            hb_coord++;
        }
    }
}

/* ---------- Responder: alarm dispatch (Core 1) ---------- */
static void responder_task(void *arg)
{
    uint32_t last_seen_arr = 0;
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n == 0) continue;
        int64_t t0 = esp_timer_get_time();

        alarms_dispatched++;
        hb_resp++;

        uint32_t arr = arrhythmia_count;
        if (arr != last_seen_arr) {
            last_seen_arr = arr;
            ESP_LOGW(TAG, "[alarm] ARRHYTHMIA ALERT dispatched (event #%lu, seq=%lu)",
                     (unsigned long)arr, (unsigned long)last_arrhythmia_seq);
        }
        wcet_update(&wcet_resp_us, t0);
    }
}

/* ---------- Freshness watchdog (Core 1): graceful degradation ----------
 * Soft-real-time deadline monitor. If no fresh sample has arrived within
 * FRESHNESS_DEADLINE_MS the sensor is presumed lost: the system fails SAFE
 * into a persistent ALARM/DEGRADED state instead of silently trusting stale
 * data. It recovers automatically when fresh samples resume.
 */
static void watchdog_task(void *arg)
{
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t age = now - last_sample_ms;

        if (age > FRESHNESS_DEADLINE_MS) {
            if (sys_state == STATE_NORMAL) {
                sys_state = STATE_DEGRADED;
                degrade_events++;
                ESP_LOGE(TAG, "[watchdog] FRESHNESS DEADLINE MISSED (age=%lums) "
                              "-> DEGRADED, failing safe to persistent ALARM",
                              (unsigned long)age);
            }
        } else {
            if (sys_state == STATE_DEGRADED) {
                sys_state = STATE_NORMAL;
                ESP_LOGW(TAG, "[watchdog] fresh data restored -> NORMAL");
            }
        }

        hb_wdog++;
        vTaskDelay(pdMS_TO_TICKS(100));   /* 10 Hz deadline check */
    }
}

/* ---------- Button ISR — toggle the injected sensor fault ---------- */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 200000) return;   /* 200 ms debounce */
    last_edge_us = now;
    sensor_fault = !sensor_fault;              /* atomic bool store */
}

/* =========================================================================
 *  Latency benchmark (RUN_LATENCY_BENCH=1) — App 3 helper pattern.
 * ========================================================================= */
#if RUN_LATENCY_BENCH
#include <limits.h>
#define BENCH_N 64
static SemaphoreHandle_t bench_sem;
static TaskHandle_t      bench_waiter;
static volatile int64_t  bench_t0;
static int64_t           sem_dt[BENCH_N];
static int64_t           ntf_dt[BENCH_N];

static void bench_waiter_task(void *arg)
{
    for (int i = 0; i < BENCH_N; i++) {
        xSemaphoreTake(bench_sem, portMAX_DELAY);
        sem_dt[i] = esp_timer_get_time() - bench_t0;
    }
    for (int i = 0; i < BENCH_N; i++) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ntf_dt[i] = esp_timer_get_time() - bench_t0;
    }
    int64_t smin=INT64_MAX, smax=0, ssum=0, nmin=INT64_MAX, nmax=0, nsum=0;
    for (int i = 0; i < BENCH_N; i++) {
        if (sem_dt[i]<smin) smin=sem_dt[i]; if (sem_dt[i]>smax) smax=sem_dt[i]; ssum+=sem_dt[i];
        if (ntf_dt[i]<nmin) nmin=ntf_dt[i]; if (ntf_dt[i]>nmax) nmax=ntf_dt[i]; nsum+=ntf_dt[i];
    }
    ESP_LOGW(TAG, "[bench] binary-semaphore  wake  min=%lld avg=%lld max=%lld us",
             (long long)smin, (long long)(ssum/BENCH_N), (long long)smax);
    ESP_LOGW(TAG, "[bench] task-notification wake  min=%lld avg=%lld max=%lld us",
             (long long)nmin, (long long)(nsum/BENCH_N), (long long)nmax);
    vTaskDelete(NULL);
}
static void bench_signaler_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    for (int i=0;i<BENCH_N;i++){ bench_t0=esp_timer_get_time(); xSemaphoreGive(bench_sem); vTaskDelay(pdMS_TO_TICKS(5)); }
    for (int i=0;i<BENCH_N;i++){ bench_t0=esp_timer_get_time(); xTaskNotifyGive(bench_waiter); vTaskDelay(pdMS_TO_TICKS(5)); }
    vTaskDelete(NULL);
}
static void run_latency_bench(void)
{
    bench_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(bench_waiter_task,   "bench_wait", 4096, NULL, 20, &bench_waiter, APP_CPU_NUM);
    xTaskCreatePinnedToCore(bench_signaler_task, "bench_sig",  4096, NULL,  6, NULL,          APP_CPU_NUM);
}
#endif

/* =========================================================================
 *  Observability plane (Core 0)
 * ========================================================================= */
#if USE_WEBSERVER
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""
#define WIFI_CHANNEL 6

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)          esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}
static void wifi_init_sta(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    wifi_config_t wc = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, .channel = WIFI_CHANNEL } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}
static esp_err_t root_get_handler(httpd_req_t *req)
{
    UBaseType_t depth = uxQueueMessagesWaiting(data_q);
    EventBits_t bits  = xEventGroupGetBits(evt_group);
    const char *state = (sys_state == STATE_NORMAL) ? "NORMAL" : "DEGRADED (fail-safe alarm)";
    const char *color = (sys_state == STATE_NORMAL) ? "#00c9a7" : "#ff5470";

    char buf[1500];
    int n = snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='1'>"
        "<title>SENTINEL - cardiac telemetry</title>"
        "<style>body{font-family:sans-serif;background:#0b1f3a;color:#e8eef5;padding:24px}"
        "h1{color:#00a896;margin-bottom:2px}.state{font-size:20px;font-weight:bold;color:%s;margin:8px 0 16px}"
        "table{border-collapse:collapse}td{padding:6px 16px;border-bottom:1px solid #1e3a5f}"
        "td:last-child{color:#00c9a7;font-weight:bold;text-align:right}</style></head><body>"
        "<h1>SENTINEL &mdash; cardiac telemetry</h1>"
        "<div class='state'>STATE: %s</div>"
        "<table>"
        "<tr><td>Sensor fault injected</td><td>%s</td></tr>"
        "<tr><td>Queue depth</td><td>%u / 8</td></tr>"
        "<tr><td>Event bits</td><td>0x%02X</td></tr>"
        "<tr><td>Heartbeats s/d/c/a/w</td><td>%lu/%lu/%lu/%lu/%lu</td></tr>"
        "<tr><td>WCET sampler / detector (us)</td><td>%lu / %lu</td></tr>"
        "<tr><td>Dropped samples</td><td>%lu</td></tr>"
        "<tr><td>Arrhythmias detected</td><td>%lu</td></tr>"
        "<tr><td>Alarms dispatched</td><td>%lu</td></tr>"
        "<tr><td>Degrade events (failed safe)</td><td>%lu</td></tr>"
        "</table></body></html>",
        color, state, sensor_fault ? "YES" : "no",
        (unsigned)depth, (unsigned)bits,
        (unsigned long)hb_prod,(unsigned long)hb_cons,(unsigned long)hb_coord,
        (unsigned long)hb_resp,(unsigned long)hb_wdog,
        (unsigned long)wcet_prod_us,(unsigned long)wcet_cons_us,
        (unsigned long)dropped_samples,(unsigned long)arrhythmia_count,
        (unsigned long)alarms_dispatched,(unsigned long)degrade_events);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}
static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root = { .uri="/", .method=HTTP_GET, .handler=root_get_handler };
        httpd_register_uri_handler(server, &root);
    }
    return server;
}
static void webmonitor_task(void *arg)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_sta();
    vTaskDelay(pdMS_TO_TICKS(3000));
    start_webserver();
    ESP_LOGI(TAG, "[webmon] HTTP server up on port 80");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
#else
static void serial_monitor_task(void *arg)
{
    for (;;) {
        UBaseType_t depth = uxQueueMessagesWaiting(data_q);
        const char *state = (sys_state == STATE_NORMAL) ? "NORMAL" : "DEGRADED";
        ESP_LOGI(TAG,
            "[monitor] STATE=%s fault=%d q=%u hb[s=%lu d=%lu c=%lu a=%lu w=%lu] "
            "wcet_us[s=%lu d=%lu c=%lu a=%lu] drop=%lu arr=%lu alarms=%lu degr=%lu",
            state, (int)sensor_fault, (unsigned)depth,
            (unsigned long)hb_prod,(unsigned long)hb_cons,(unsigned long)hb_coord,
            (unsigned long)hb_resp,(unsigned long)hb_wdog,
            (unsigned long)wcet_prod_us,(unsigned long)wcet_cons_us,
            (unsigned long)wcet_coord_us,(unsigned long)wcet_resp_us,
            (unsigned long)dropped_samples,(unsigned long)arrhythmia_count,
            (unsigned long)alarms_dispatched,(unsigned long)degrade_events);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

/* ---------- app_main ---------- */
void app_main(void)
{
    printf("\n*** SENTINEL BOOT OK — app_main reached ***\n");
    fflush(stdout);

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== SENTINEL — fail-safe cardiac telemetry (capstone) ====");
#if USE_WEBSERVER
    ESP_LOGI(TAG, "Monitor: WEB (USE_WEBSERVER=1)");
#else
    ESP_LOGI(TAG, "Monitor: SERIAL (USE_WEBSERVER=0)");
#endif

    data_q         = xQueueCreate(8, sizeof(ecg_sample_t));
    evt_group      = xEventGroupCreate();
    last_sample_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* Core 1 — real-time plane. Responder created FIRST so its handle is valid
     * before the coordinator or button can notify it. */
    xTaskCreatePinnedToCore(responder_task,   "alarm_disp",     4096, NULL, 12, &responder_handle, APP_CPU_NUM);
    xTaskCreatePinnedToCore(watchdog_task,    "freshness_wdog", 4096, NULL, 11, NULL,              APP_CPU_NUM);
    xTaskCreatePinnedToCore(coordinator_task, "coordinator",    4096, NULL,  9, NULL,              APP_CPU_NUM);
    xTaskCreatePinnedToCore(producer_task,    "ecg_sampler",    4096, NULL,  8, NULL,              APP_CPU_NUM);
    xTaskCreatePinnedToCore(consumer_task,    "arrhythmia_det", 4096, NULL,  8, NULL,              APP_CPU_NUM);

    /* Core 0 — observability plane */
#if USE_WEBSERVER
    xTaskCreatePinnedToCore(webmonitor_task,     "webmon",  4096, NULL, 4, NULL, PRO_CPU_NUM);
#else
    xTaskCreatePinnedToCore(serial_monitor_task, "monitor", 4096, NULL, 4, NULL, PRO_CPU_NUM);
#endif

    /* Button -> ISR -> toggle injected sensor fault (drives the degradation demo) */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);

#if RUN_LATENCY_BENCH
    run_latency_bench();
#endif
}
