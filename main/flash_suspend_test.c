/*
 * auto-suspend の受け入れ試験 — 「静かに壊れないこと」を明確な失敗に変える。
 *
 * `CONFIG_SPI_FLASH_AUTO_SUSPEND` は、フラッシュの erase/program 中に
 * キャッシュ読みが来たらハードウェアが書き込みを中断して読みを通す機能。
 * これが入ると IDF は `spi_flash_disable_interrupts_caches_and_other_cpu()`
 * を**コンパイルすらしなくなる** (esp-idf の spi_flash_os_func_app.c:32 の
 * `SPI_FLASH_CACHE_NO_DISABLE`)。両コアの停止が消えるので、打鍵レイテンシの
 * 最大要因が構造的に無くなる。
 *
 * だが代償として、新しい壊れ方が入りうる。`SPI_FLASH_SUSPEND_TSUS_VAL_US`
 * (suspend コマンドを出してからキャッシュが読んでよいまでの待ち) が実チップの
 * 必要時間より短いと、**まだ busy のうちにキャッシュが読んで、黙ってゴミを
 * 受け取る**。命令なら異常動作、rodata なら間違ったグリフや間違った辞書行。
 * フラッシュ書き込み中のキャッシュミスでしか起きないので、通常の動作確認では
 * 絶対に見つからない。
 *
 * だからこの試験を焼く前に通す。やることは 1 つだけ:
 *
 *   core 0 で littlefs に書き続け (= erase + program を延々と起こし)、
 *   同時に core 1 でフラッシュ常駐データを CRC し続けて、値がぶれないか見る。
 *
 * 不一致が 1 件でも出たら tSUS を上げて再試験。それでも出るなら auto-suspend を
 * 諦める。0 件なら「停止」の章を閉じてよい。
 *
 * CRC の対象に jisyo パーティションを選んでいるのは、**mmap したフラッシュを
 * 読む経路が実際に本番にある唯一のもの**だから (docs/skk-ime-design.md §6.7 —
 * 辞書はコピーもロードもせず mmap をそのまま読む)。フォントの .rodata も
 * 同じ経路だが、あちらは static でこの TU から掴めない。辞書が無いビルドでは
 * factory パーティション (アプリ自身のイメージ) に落ちる。
 *
 * CRC 関数は ROM 上にあるので、試験そのものはキャッシュ停止の影響を受けない。
 * 一方このファイルのコードは flash に居るので、**試験が最後まで走ったこと
 * 自体が、命令フェッチ側も無事だったことの証拠**になる。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "sdkconfig.h"

#include "flash_suspend_test.h"

#if CONFIG_MQJS_FLASH_SUSPEND_TEST

static const char *TAG = "fsusp";

/* CRC を回す窓。大きいほどデータキャッシュを追い出してフラッシュを実際に
   読みに行くので、試験としては大きい方が強い。1MB なら 竹 (1.86MB) でも
   収まる。 */
#define WINDOW_BYTES (1024 * 1024)

/* littlefs へ 1 回に書く量。littlefs のブロックは 4KB なので、これで
   毎回複数セクタの erase + program が起きる。 */
#define WRITE_BYTES  (16 * 1024)
#define TEST_PATH    "/littlefs/fsusp.bin"

static const void          *s_window;
static esp_partition_mmap_handle_t s_map;
static uint32_t             s_ref_crc;
static volatile bool        s_writing_done;
static volatile uint32_t    s_crc_iters;
static volatile uint32_t    s_crc_bad;
static volatile uint32_t    s_writes;

/* core 1: 窓を CRC し続け、起動時に取った基準値と比べる。
   ここが本体。読みながら書かれても値が変わらないことを確かめている。 */
static void crc_task(void *arg)
{
    (void)arg;
    while (!s_writing_done) {
        uint32_t c = esp_rom_crc32_le(0, s_window, WINDOW_BYTES);
        s_crc_iters++;
        if (c != s_ref_crc) {
            s_crc_bad++;
            /* 静かな破損を明確な失敗にするのが目的なので、黙らない。 */
            ESP_LOGE(TAG, "MISMATCH #%lu: got %08lx want %08lx (iter %lu)",
                     (unsigned long)s_crc_bad, (unsigned long)c,
                     (unsigned long)s_ref_crc, (unsigned long)s_crc_iters);
        }
        /* 少しだけ譲る。譲らないと core 1 の他のタスクが飢える。 */
        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

/* core 0: littlefs に書き続けて erase/program を起こす。実際の
   オートセーブが通るのと同じ経路をわざと踏んでいる (生の flash API を
   直接叩くより、本番に近い方が試験として意味がある)。 */
static void write_task(void *arg)
{
    (void)arg;
    char *buf = malloc(WRITE_BYTES);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the write buffer");
        s_writing_done = true;
        vTaskDelete(NULL);
    }
    for (int i = 0; i < CONFIG_MQJS_FLASH_SUSPEND_TEST_ITERS; i++) {
        /* 毎回中身を変える。同じ内容だと littlefs が書かずに済ませうる。 */
        memset(buf, (int)(i & 0xff), WRITE_BYTES);
        FILE *f = fopen(TEST_PATH, "wb");
        if (!f) {
            ESP_LOGE(TAG, "cannot open %s", TEST_PATH);
            break;
        }
        fwrite(buf, 1, WRITE_BYTES, f);
        fflush(f);
        fclose(f);
        s_writes++;
        if ((i % 25) == 0)
            ESP_LOGI(TAG, "writes=%lu crc_iters=%lu bad=%lu",
                     (unsigned long)s_writes, (unsigned long)s_crc_iters,
                     (unsigned long)s_crc_bad);
    }
    free(buf);
    remove(TEST_PATH);
    s_writing_done = true;

    /* 判定は 1 行で読めるようにする。 */
    ESP_LOGW(TAG, "==== RESULT: writes=%lu crc_iters=%lu mismatches=%lu -> %s",
             (unsigned long)s_writes, (unsigned long)s_crc_iters,
             (unsigned long)s_crc_bad,
             s_crc_bad ? "FAIL (raise TSUS or drop auto-suspend)" : "PASS");
    ESP_LOGW(TAG, "==== auto_suspend=%d tsus=%dus",
#if CONFIG_SPI_FLASH_AUTO_SUSPEND
             1, CONFIG_SPI_FLASH_SUSPEND_TSUS_VAL_US
#else
             0, 0
#endif
             );
    vTaskDelete(NULL);
}

void flash_suspend_test_start(void)
{
    /* 本番で mmap を読む唯一の経路が辞書なので、そこを見る。 */
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "jisyo");
    if (!p || p->size < WINDOW_BYTES) {
        /* 辞書が無いビルドでも試験は成立させる: アプリ自身のイメージを見る。
           こちらもフラッシュ常駐で、壊れれば同じように困る。 */
        p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                     ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    }
    if (!p) {
        ESP_LOGE(TAG, "no partition to watch");
        return;
    }

    esp_err_t err = esp_partition_mmap(p, 0, WINDOW_BYTES,
                                       ESP_PARTITION_MMAP_DATA,
                                       &s_window, &s_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap('%s'): %s", p->label, esp_err_to_name(err));
        return;
    }

    /* 基準値は書き込みを始める前に取る。 */
    s_ref_crc = esp_rom_crc32_le(0, s_window, WINDOW_BYTES);
    ESP_LOGW(TAG, "watching '%s' %d bytes, ref crc %08lx, %d write iters",
             p->label, WINDOW_BYTES, (unsigned long)s_ref_crc,
             CONFIG_MQJS_FLASH_SUSPEND_TEST_ITERS);

    /* CRC は core 1 (UI 側)、書き込みは core 0 (JS 側)。本番の役割分担と
       同じ配置にしておかないと、他コア停止が起きるかどうかを見られない。
     *
     * 優先度は IDF の auto-suspend ドキュメントの指示に従う: 消去タスクより
     * **低い**優先度のコードは消去中に走らせてはいけない (走らせると
     * suspend が resume を追い越して CPU が飢える)。前回は両方 prio 3 で
     * 走らせており、これはドキュメントが禁じている形だった。
     *
     * ただしこの配置は同時に、auto-suspend が我々の用途に**そもそも合わない**
     * ことも測る: 描画タスクを消去より低優先にするということは、消去中は
     * 描画しないということで、それは消したかった停止そのもの。crc_iters が
     * auto-suspend 無効時 (100) まで落ちるなら、得るものが無いと分かる。 */
    xTaskCreatePinnedToCore(crc_task, "fsusp_crc", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(write_task, "fsusp_wr", 4096, NULL, 10, NULL, 0);
}

#else  /* !CONFIG_MQJS_FLASH_SUSPEND_TEST */

void flash_suspend_test_start(void) {}

#endif
