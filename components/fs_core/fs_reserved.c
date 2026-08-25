/*
 * 変更を禁じるサブツリー。
 *
 * これは権限ではなく不変条件なので、fs_core の内側に置いてある。
 * grant (docs/filer-storage-design.md §7) は「どこまで書いてよいか」を
 * 広げる仕組みで、ユーザが同意画面で "/internal" を丸ごと許すことは
 * 起こりうる —— 同意画面の文言は「内蔵への書き込み」としか言わない。
 * だが main/storage.c 冒頭が明言しているとおり、/littlefs/apps/ の
 * スクリプトが次回起動時に無検証で走れるのは、**そこへ届く経路が署名
 * 検証済みしか無い**からだけである。grant で書けてしまうと、手元で
 * 書いた未署名のコードが署名済みアプリの顔をして走る。
 *
 * だから「一番広い grant でも通らない」層が要る。それがここ。
 *
 * ESP-IDF のヘッダを 1 つも含まないのは意図的で、境界文字の判定
 * ("/internal/appsfoo" は中ではない) をホストの gcc で直接叩けるように
 * するため —— pc_test/test_reserved.c がこの .c をそのままコンパイルする。
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* 表にしてあるのは、後から増えるのが分かっているから (署名済み辞書、
   工場出荷時の設定、…)。今は 1 本。
 *
 * 仮想パスで書く: 実 VFS パス ("/littlefs/apps") ではなくボリューム id
 * 込みの "/internal/apps" にしておくと、内蔵の実マウント先を変えても
 * この表を書き換えずに済む (§4)。
 *
 * "/sd/apps" は**入れない**。カードのアプリは使うたびに crypto_sign_open
 * で検証されるので、書き換えられても署名が合わなくなるだけ。逆にカードの
 * ファイル操作をファイラから禁じると、配布用のカードを端末で作れなくなる。 */
static const char *const s_reserved[] = {
    "/internal/apps",
};

/* vpath が予約サブツリーの中か。
 *
 * 呼び出し側は fsvol_resolve で形を検査済みのパスを渡す契約
 * (path_shape_ok が "." だけのセグメント・制御文字・"//" を弾くので
 * ".." は生き残れない)。だから正規化なしの前方一致で足りる。
 *
 * 前方一致の直後の 1 文字を見るのが要点で、strncmp だけで済ませると
 * "/internal/appsfoo" が中だと判定される。境界は '\0' か '/' しかない。 */
bool fs_path_reserved(const char *vpath)
{
    if (!vpath)
        return false;
    for (size_t i = 0; i < sizeof s_reserved / sizeof s_reserved[0]; i++) {
        size_t n = strlen(s_reserved[i]);
        if (strncmp(vpath, s_reserved[i], n) != 0)
            continue;
        char c = vpath[n];
        if (c == '\0' || c == '/')
            return true;
    }
    return false;
}
