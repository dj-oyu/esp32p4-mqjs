/*
 * SDMMC コントローラを microSD と esp-hosted (C6 Wi-Fi) で分け合う。
 *
 * P4 の SDMMC は「1 コントローラ + 2 スロット」で、Tab5 では
 * slot 0 = microSD、slot 1 = C6 という配線になっている (設計 §1.3)。
 * ピンは競合しない —— にもかかわらず両方は上がらなかった。
 *
 * 理由は IDF 6 のドライバ側にある。legacy の `sdmmc_host_init()` は
 * 呼ばれるたびに **無条件で新しいコントローラを作ろうとする**:
 *
 *     esp_err_t sdmmc_host_init(void) {
 *         return sd_host_create_sdmmc_controller(&cfg, &s_ctlr);
 *     }
 *
 * ハードのコントローラは 1 個しかないので、2 人目の
 * `sd_host_claim_controller()` は必ず落ちる。スロットを足す
 * `sdmmc_host_init_slot()` の方は同じ静的な `s_ctlr` に対して
 * s_slot0 / s_slot1 を並べられる作りなので、**足りないのは
 * 「もう在るなら作らない」という一行だけ**。
 *
 * 実機ではこれがブートループとして出た: app_main の sdcard_init が先に
 * コントローラを取り、後から上がる esp_hosted が
 *   E SD_HOST: no available sd host controller
 *   E H_SDIO_DRV: could not create sdio handle, exiting
 * で落ちて再起動、を繰り返す。SD 側は掴めているので、ログだけ見ると
 * 「SD は動いているのに Wi-Fi が死ぬ」という見え方になる。
 *
 * 直し方は 2 つあった。(a) 起動順を入れ替えて必ず esp_hosted に先に
 * 取らせる、(b) 生成を冪等にする。(a) は「Wi-Fi を切ったビルドでは
 * 誰も作らない」「先に取った方が勝つ」という順序依存を残すので採らない。
 * ここは (b) —— リンカの --wrap で参照カウントを噛ませ、**どちらが
 * 先に呼んでも 2 人目は同じコントローラを共有する**。managed_components
 * の esp_hosted には一切手を入れない (ui_tab5 が esp_hosted_init に、
 * main が esp_panic_handler にやっているのと同じ手)。
 *
 * deinit は wrap しない。スロット単位で外す道 (SDMMC_HOST_FLAG_DEINIT_ARG
 * + sdmmc_host_deinit_slot) が用意されていて、SD 側はそれを使う
 * (sdcard.c)。esp_hosted がコントローラごと畳んだ場合は SD のスロットも
 * 道連れになるが、それは「カードが消えた」と同じ形で probe() が拾い、
 * fs_core が自動でアンマウントする —— 抜き挿しのために既に書いてある
 * 経路がそのまま面倒を見る (設計 §6)。
 */
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_MQJS_SDCARD

esp_err_t __real_sdmmc_host_init(void);

static const char *TAG = "sdmmc_share";
static int s_refs;

esp_err_t __wrap_sdmmc_host_init(void)
{
    if (s_refs > 0) {
        /* 既に誰か (SD か C6 か) が作ってある。legacy ドライバの s_ctlr は
           両者で共有される静的変数なので、このまま
           sdmmc_host_init_slot() が自分のスロットを足せる。 */
        s_refs++;
        ESP_LOGI(TAG, "sharing the SDMMC controller (%d users)", s_refs);
        return ESP_OK;
    }
    esp_err_t err = __real_sdmmc_host_init();
    if (err == ESP_OK)
        s_refs = 1;
    return err;
}

#endif /* CONFIG_MQJS_SDCARD */
