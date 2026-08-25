/*
 * フラッシュ停止の実測 (native-editor-spec.md M0 / native-editor-design.md §6.4)。
 *
 * フラッシュの program/erase 中、IDF はキャッシュを落として**もう一方のコアを
 * IRAM のスピンループに閉じ込める**。docs/native-editor-design.md §6.1 の実測で、
 * これは core 1 のスループットを約 4 割削る。
 *
 * だが「4 割削る」が痛いかどうかは、**停止が打鍵と重なるかどうか**で決まる。
 * エディタのオートセーブはアイドル駆動にするので、重なる頻度は低いかもしれない。
 * XIP + jisyo の PSRAM コピー (PSRAM 12MB + 起動 1.3 秒) を払う価値があるかは、
 * その頻度を測ってからでないと決められない —— それがこの計測器の存在理由。
 *
 * 測り方は `--wrap`。ui_tab5 が esp_hosted_init に、main が esp_panic_handler に
 * 使っているのと同じ手で、停止の入口と出口を挟む。別 TU なので効く。
 *
 * **この中のコードはキャッシュが落ちた状態で走る。** だから:
 *   - IRAM_ATTR (フラッシュから命令を取れない)
 *   - 内部 SRAM の変数だけ (PSRAM は生きているが、わざわざ触らない)
 *   - 時計は CSR 読み (esp_timer はタイマ周辺機器を触るので避ける)
 *   - ログを出さない (出力経路がフラッシュを踏む)
 * 集計と報告は停止の外でやる。
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "flash_stall_meter.h"
#include "mqjs_runtime.h"

#if CONFIG_MQJS_FLASH_STALL_METER

/* P4 は 360MHz 固定 (CPU 周波数を上げない、というユーザ決定)。サイクル数を
   µs に直すのはこの定数だけ。 */
#define CPU_MHZ 360

/* 「打鍵中」の定義。最終打鍵からこの時間内に始まった停止を「重なった」と
   数える。オートセーブのアイドル閾値 (2 秒) より短くしてあるので、
   エディタ自身の書き込みは定義上ここに入らない —— 入ってしまうと
   「自分で作った停止」を数えることになり、判断材料にならない。 */
#define TYPING_WINDOW_US 1500000

extern void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
extern void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);

static const char *TAG = "fstall";

/* すべて内部 SRAM (BSS)。停止中に触るのでフラッシュにあってはならない。 */
/* **コアごとに持つ。** `esp_cpu_get_cycle_count()` はそのコアの CSR を読む
   もので、2 つのコアのカウンタは同期していない。1 つの静的変数で入口と出口を
   挟むと、別のコアで測った値どうしを引き算してしまい、初回の計測で
   「1 回の停止が 10.18 秒」という有り得ない最大値が出た。 */
static DRAM_ATTR volatile uint32_t s_depth[2];
static DRAM_ATTR volatile uint32_t s_t_enter[2];  /* サイクル */
/* 引き算が壊れた回数。黙って捨てると「計測器は正しい」と誤読するので数える。
   360MHz の 32bit カウンタは約 11.9 秒で一周するので、それもここに落ちる。 */
static DRAM_ATTR volatile uint32_t s_bogus;
static DRAM_ATTR volatile uint32_t s_n;          /* 停止回数 */
static DRAM_ATTR volatile uint32_t s_n_typing;   /* うち打鍵に重なった回数 */
static DRAM_ATTR volatile uint32_t s_us_total;
static DRAM_ATTR volatile uint32_t s_us_max;
/* log2 バケツ: [0]=<64us [1]=<128 ... [9]=>=16ms。分布を見ないと
   「1 回 30ms が 1 発」と「1ms が 30 発」を区別できない。 */
static DRAM_ATTR volatile uint32_t s_hist[10];

void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void)
{
    uint32_t c = esp_cpu_get_core_id();
    if (s_depth[c]++ == 0)
        s_t_enter[c] = esp_cpu_get_cycle_count();
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
}

void IRAM_ATTR __wrap_spi_flash_enable_interrupts_caches_and_other_cpu(void)
{
    uint32_t c = esp_cpu_get_core_id();
    __real_spi_flash_enable_interrupts_caches_and_other_cpu();
    if (s_depth[c] == 0 || --s_depth[c] != 0)
        return;                       /* 対になっていない enable は無視 */

    /* ここから先はキャッシュが戻っているので、フラッシュに触ってよい。 */
    uint32_t us = (esp_cpu_get_cycle_count() - s_t_enter[c]) / CPU_MHZ;
    /* 1 回の停止が 100ms を超えることは無い (実測の最悪でも数十 ms)。
       超えたらカウンタの一周か、対の取り違え。平均と最大を汚さないよう
       別に数える。 */
    if (us > 100000) {
        s_bogus++;
        return;
    }
    s_n++;
    s_us_total += us;
    if (us > s_us_max)
        s_us_max = us;

    uint32_t b = 0, v = us >> 6;      /* 64us を 1 単位に */
    while (v && b < 9) {
        v >>= 1;
        b++;
    }
    s_hist[b]++;

    /* 打鍵と重なったか。最終打鍵の時刻は mqjs 側が既に持っている。 */
    int64_t last = mqjs_last_key_us();
    if (last > 0 && (mqjs_now_us() - last) < TYPING_WINDOW_US)
        s_n_typing++;
}

void flash_stall_meter_report(const char *why)
{
    if (!s_n) {
        ESP_LOGW(TAG, "%s: no flash stalls at all", why ? why : "report");
        return;
    }
    ESP_LOGW(TAG, "==== %s: stalls=%lu typing=%lu total=%lums max=%luus avg=%luus",
             why ? why : "report",
             (unsigned long)s_n, (unsigned long)s_n_typing,
             (unsigned long)(s_us_total / 1000), (unsigned long)s_us_max,
             (unsigned long)(s_us_total / (s_n ? s_n : 1)));
    if (s_bogus)
        ESP_LOGE(TAG, "==== %lu bogus deltas discarded (meter bug or wrap)",
                 (unsigned long)s_bogus);
    ESP_LOGW(TAG, "==== hist us <64 <128 <256 <512 <1k <2k <4k <8k <16k 16k+");
    ESP_LOGW(TAG, "====      %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
             (unsigned long)s_hist[0], (unsigned long)s_hist[1],
             (unsigned long)s_hist[2], (unsigned long)s_hist[3],
             (unsigned long)s_hist[4], (unsigned long)s_hist[5],
             (unsigned long)s_hist[6], (unsigned long)s_hist[7],
             (unsigned long)s_hist[8], (unsigned long)s_hist[9]);
}

void flash_stall_meter_reset(void)
{
    s_n = s_n_typing = s_us_total = s_us_max = s_bogus = 0;
    for (int i = 0; i < 10; i++)
        s_hist[i] = 0;
}

/* 定期報告。数が動いたときだけ出す —— 静かなときに毎分ログを吐くと、
   本当に何も起きていないのか計測器が死んでいるのか分からなくなるので、
   最初の 1 回だけは 0 でも出す。 */
static void meter_task(void *arg)
{
    (void)arg;
    uint32_t last = 0xffffffffu;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (s_n != last) {
            last = s_n;
            flash_stall_meter_report("30s");
        }
    }
}

void flash_stall_meter_start(void)
{
    ESP_LOGW(TAG, "counting flash cache stalls (typing window %d ms)",
             TYPING_WINDOW_US / 1000);
    xTaskCreatePinnedToCore(meter_task, "fstall", 3072, NULL, 1, NULL, 0);
}

#else  /* !CONFIG_MQJS_FLASH_STALL_METER */

void flash_stall_meter_start(void) {}
void flash_stall_meter_report(const char *why) { (void)why; }
void flash_stall_meter_reset(void) {}

#endif
