/*
 * microSD を fs_core の "sd" ボリュームとして登録する。
 *
 * ピン・スロット・LDO チャンネルはすべて Kconfig から来る。ボード固有の
 * 知識をこの C に一切書かないので、Stamp-P4 との差は「Kconfig が off か
 * on か」の一点に縮む (docs/filer-storage-design.md §3)。
 *
 * off のときもこの TU はコンパイルされ、sdcard_init() は「登録しない」を
 * 返すだけの実装になる。呼び出し側 (app_main) に #ifdef を書かせないため。
 * 実体が無ければリンカが丸ごと落とすので、flash は増えない。
 */
#include "sdcard.h"

#include "esp_log.h"
#include "fs_core.h"

static const char *TAG = "sdcard";

#if CONFIG_MQJS_SDCARD

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT "/sd"

static sdmmc_card_t         *s_card;
static sd_pwr_ctrl_handle_t  s_pwr;

static esp_err_t sd_mount(const fsvol_t *v)
{
    (void)v;

#if CONFIG_MQJS_SDCARD_LDO_CHAN != 0
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {
            .ldo_chan_id = CONFIG_MQJS_SDCARD_LDO_CHAN,
        };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LDO ch%d: %s", CONFIG_MQJS_SDCARD_LDO_CHAN,
                     esp_err_to_name(err));
            return err;
        }
    }
#endif

    sdmmc_host_t host  = SDMMC_HOST_DEFAULT();
    host.slot          = CONFIG_MQJS_SDCARD_SLOT;
    host.max_freq_khz  = CONFIG_MQJS_SDCARD_FREQ_KHZ;
    host.pwr_ctrl_handle = s_pwr;
    /* アンマウントはコントローラごとではなく **自分のスロットだけ** 畳む。
       既定の deinit は s_slot0/s_slot1 を両方外すので、そのまま呼ぶと
       カードを取り出した拍子に C6 の SDIO リンク (slot 1) まで切れる。
       スロット単位の道が用意されているのでそちらを使う。 */
    host.flags        |= SDMMC_HOST_FLAG_DEINIT_ARG;
    host.deinit_p      = sdmmc_host_deinit_slot;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = CONFIG_MQJS_SDCARD_WIDTH;
    slot.clk   = CONFIG_MQJS_SDCARD_PIN_CLK;
    slot.cmd   = CONFIG_MQJS_SDCARD_PIN_CMD;
    slot.d0    = CONFIG_MQJS_SDCARD_PIN_D0;
#if CONFIG_MQJS_SDCARD_WIDTH == 4
    slot.d1    = CONFIG_MQJS_SDCARD_PIN_D1;
    slot.d2    = CONFIG_MQJS_SDCARD_PIN_D2;
    slot.d3    = CONFIG_MQJS_SDCARD_PIN_D3;
#endif
    /* Tab5 は CD/WP を配線していない (公式 BSP も GPIO_NUM_NC)。 */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        /* 絶対に自動フォーマットしない。読めないカードの正体はたいてい
           「別の機器の大事なデータ」であって、空にしてよい理由にならない。 */
        .format_if_mount_failed = false,
        .max_files              = CONFIG_MQJS_SDCARD_MAX_FILES,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &cfg, &s_card);
    if (err != ESP_OK) {
        /* カードが入っていないだけなら日常茶飯事なので警告どまり。 */
        ESP_LOGW(TAG, "mount: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    ESP_LOGI(TAG, "%s: %lluMB %s", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20,
             s_card->is_sdio ? "SDIO" : (s_card->is_mmc ? "MMC" : "SD"));
    return ESP_OK;
}

static esp_err_t sd_unmount(const fsvol_t *v)
{
    (void)v;
    esp_err_t err = ESP_OK;
    if (s_card) {
        err = esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
        s_card = NULL;
    }
    /* LDO ハンドルは持ち越さない。挿し直しのたびに new すると
       チャンネルを取り切って二度と開けなくなる。 */
    if (s_pwr) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        s_pwr = NULL;
    }
    return err;
}

/* 検出ピンが無いので、カードに直接聞く (§6)。抜けていれば SDMMC の
   コマンドがタイムアウトし、fs_core が自動でアンマウントに回す。 */
static bool sd_probe(const fsvol_t *v)
{
    (void)v;
    return s_card && sdmmc_get_status(s_card) == ESP_OK;
}

static esp_err_t sd_usage(const fsvol_t *v, uint64_t *total, uint64_t *freeb)
{
    (void)v;
    return esp_vfs_fat_info(SD_MOUNT, total, freeb);
}

static const fsvol_ops_t s_ops = {
    .mount   = sd_mount,
    .unmount = sd_unmount,
    .probe   = sd_probe,
    .usage   = sd_usage,
};

static const fsvol_t s_vol = {
    .id     = "sd",
    .label  = "microSD",
    .root   = SD_MOUNT,
    .fstype = "fat",
    .flags  = FSVOL_REMOVABLE,
    .ops    = &s_ops,
};

bool sdcard_init(void)
{
    if (fsvol_register(&s_vol) != ESP_OK) {
        ESP_LOGE(TAG, "volume registration failed");
        return false;
    }
    /* 起動時に入っていれば使えるようにしておく。入っていなくても
       登録は生きているので、後から挿して「マウント」できる。 */
    return fsvol_mount(&s_vol) == ESP_OK;
}

#else  /* !CONFIG_MQJS_SDCARD */

bool sdcard_init(void)
{
    ESP_LOGD(TAG, "not built in");
    return false;
}

#endif
