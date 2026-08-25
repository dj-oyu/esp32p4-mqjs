/*
 * test_reserved.c — fs_path_reserved() の境界。
 *
 * この述語ひとつが「grant では開けられない層」の全部なので、間違えたら
 * 静かに穴が開く。狙って落としに来ているのは前方一致の直後の 1 文字で、
 *
 *     strncmp(path, "/internal/apps", 14) == 0
 *
 * と書くと "/internal/appsfoo" までが予約扱いになり、ユーザが作った
 * ただのフォルダが二度と消せなくなる (逆向きの事故: 安全側に倒れるので
 * 気づかれにくい)。境界は '\0' か '/' しかない。
 *
 * fs_reserved.c は ESP-IDF のヘッダを 1 つも含まないので、この suite は
 * その .c だけをコンパイルして直接叩ける。宣言をここに書き写しているのも
 * 同じ理由 (fs_core.h は esp_err.h を引く)。
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

bool fs_path_reserved(const char *vpath);

static int t_checks;
static int t_fails;

static void expect(const char *path, bool want)
{
    bool got = fs_path_reserved(path);
    t_checks++;
    if (got != want) {
        t_fails++;
        printf("FAIL  %-28s expected %-9s actual %s\n",
               path ? path : "(null)",
               want ? "RESERVED" : "open", got ? "RESERVED" : "open");
    } else {
        printf("ok    %-28s %s\n", path ? path : "(null)",
               want ? "RESERVED" : "open");
    }
}

int main(void)
{
    printf("=== fs_path_reserved ===\n");

    /* 予約サブツリーそのものと、その下 */
    expect("/internal/apps",          true);
    expect("/internal/apps/foo.js",   true);
    expect("/internal/apps/sub/deep.js", true);
    /* 末尾の '/'。fsvol_resolve は落としてから実パスを組むが、予約判定は
       落とす前の仮想パスを見るので、ここで通ると素通りする。 */
    expect("/internal/apps/",         true);

    /* 前方一致だけで判定したときに漏れる形。ここが本命。 */
    expect("/internal/appsfoo",       false);
    expect("/internal/appsfoo/x",     false);
    expect("/internal/app",           false);

    /* 同じ内蔵の別の場所 */
    expect("/internal/data/x",        false);
    expect("/internal",               false);

    /* カードは別ボリューム。上のアプリは使うたびに署名を確かめるので
       予約しない (docs/filer-storage-design.md §13.3)。 */
    expect("/sd/apps/x.mjsa",         false);

    /* id は他所でも大文字小文字を区別して比較している (fsvol_find の
       strcmp)。ここも同じにしておく —— 大文字で書いても "INTERNAL" と
       いうボリュームは存在しないので、そもそも resolve で落ちる。 */
    expect("/INTERNAL/APPS",          false);

    /* 呼び出し側が壊れていても落ちない */
    expect(NULL,                      false);
    expect("",                        false);
    expect("/",                       false);

    printf("%s  %d checks, %d failures\n",
           t_fails ? "SUITE-FAIL" : "SUITE-OK", t_checks, t_fails);
    return t_fails ? 1 : 0;
}
