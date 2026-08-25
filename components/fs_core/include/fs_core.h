/*
 * fs_core — ボリューム登録簿と、その上のボード非依存なファイル操作。
 *
 * 設計の中心は docs/filer-storage-design.md §2:
 *
 *     Stamp-P4 は「カードが永久に入っていない Tab5」として扱う。
 *
 * ボードの差は #ifdef ではなく「登録されたボリュームの集合」という
 * データになる。この層より上 (JS バインディングもファイラも) は
 * ボード名を一度も知らない。
 *
 * 上位が触るパスは仮想パス "/<volume-id>/..." だけで、実 VFS パス
 * ("/littlefs", "/sd") はこのファイルの内側に閉じる (§4)。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 実 VFS パスの上限。LittleFS のオブジェクト名長と FATFS の LFN 255 を
   何段か重ねても収まる長さ。仮想パスも同じ上限で切る。 */
#define FS_PATH_MAX 512

/* 1 エントリ名の上限 (バイト)。FATFS の LFN は最大 255 UTF-16 単位 =
   最悪 765 バイトになりうるが、そこまで長い名前は表示もできないので
   ここで切り捨てる (切り捨てた名前で開こうとすると ENOENT になる —
   壊すより見えない方がまし)。 */
#define FS_NAME_MAX 256

/* 1 回の fs_dir_open が保持するエントリ数の上限。超えた分は捨てて
   fs_dir_truncated() が真になる。DCIM に数千枚入ったカードで
   PSRAM を食い潰さないための天井。 */
#define FS_DIR_MAX 4096

/* 再帰削除で潜る深さの上限。循環リンクは無い FS ばかりだが、壊れた
   カードで無限に潜らないための保険。 */
#define FS_RM_DEPTH_MAX 8

/* ---- ボリューム ------------------------------------------------- */

typedef struct fsvol fsvol_t;

/* マウントされていないボリュームが「なぜ」使えないのか。
 *
 * 検出ピンの無いハードでは、媒体が在るかどうかはマウントを試すまで
 * 分からない ので UNKNOWN から始まる。この区別を持たないと、未フォーマット
 * のカードが「入っていません」と表示されてしまい、直す手段が画面から
 * 消える (docs/filer-storage-design.md §12)。 */
typedef enum {
    FSVOL_ST_MOUNTED = 0,  /* 読み書きできる */
    FSVOL_ST_UNKNOWN,      /* まだ試していない */
    FSVOL_ST_ABSENT,       /* 媒体が無い (カードが入っていない) */
    FSVOL_ST_UNREADABLE,   /* 媒体は在るが、ファイルシステムが読めない */
} fsvol_state_t;

typedef struct {
    /* NULL 可。NULL の場合そのボリュームは常時マウント済みとして扱う。 */
    esp_err_t (*mount)(const fsvol_t *v);
    esp_err_t (*unmount)(const fsvol_t *v);
    /* 媒体が今も在るか。NULL なら常に在るとみなす。抜き挿し検出ピンが
       無いハードでは実際にカードへコマンドを打って確かめる (§6)。 */
    bool      (*probe)(const fsvol_t *v);
    /* 総容量と空き。NULL なら fs_usage() が ESP_ERR_NOT_SUPPORTED。 */
    esp_err_t (*usage)(const fsvol_t *v, uint64_t *total, uint64_t *freeb);
    /* マウントできていないときの理由。NULL なら ABSENT と区別しない。
       直近のマウント試行の結果から答える (§12)。 */
    fsvol_state_t (*media_state)(const fsvol_t *v);
    /* 中身を捨てて新しいファイルシステムを作る。NULL = 非対応。
       時間がかかるので JS タスクからは決して直接呼ばないこと。 */
    esp_err_t (*format)(const fsvol_t *v);
} fsvol_ops_t;

#define FSVOL_REMOVABLE 0x01  /* 抜き挿しされうる。ファイラが「取り出し」を出す */
#define FSVOL_SYSTEM    0x02  /* 内蔵。フォーマットを出さない/削除に確認を足す */

struct fsvol {
    const char *id;      /* "internal" / "sd" — 仮想パスの第一要素 */
    const char *label;   /* "内蔵" / "microSD" — 表示専用 */
    const char *root;    /* 実 VFS パス。末尾に '/' を付けない */
    const char *fstype;  /* "littlefs" / "fat" — 表示専用 */
    uint8_t     flags;
    const fsvol_ops_t *ops;
};

/* v は static 寿命でなければならない (登録簿はポインタだけ持つ)。
   同じ id を二度登録すると ESP_ERR_INVALID_STATE。 */
esp_err_t      fsvol_register(const fsvol_t *v);
int            fsvol_count(void);
const fsvol_t *fsvol_at(int i);
const fsvol_t *fsvol_find(const char *id);

/* マウント状態。ops->probe があれば、真を返す前に媒体の生存も確かめ、
   抜かれていたら自動的にアンマウントして偽を返す。 */
bool      fsvol_mounted(const fsvol_t *v);
esp_err_t fsvol_mount(const fsvol_t *v);    /* 冪等 */
esp_err_t fsvol_unmount(const fsvol_t *v);  /* 冪等 */

/* マウント世代。マウントに成功するたびに増える。抜き差しをまたいだ
   ハンドルや権限を「まだ有効か」と後から問い合わせるための番号で、
   アンマウント時にコールバックで回る必要をなくす (JS の grant は
   これを控えておいて、使うときに見比べるだけでよい)。 */
uint32_t fsvol_epoch(const fsvol_t *v);

/* マウント済みなら MOUNTED、そうでなければ ops->media_state の答え。 */
fsvol_state_t fsvol_state(const fsvol_t *v);

/* 中身を捨てて作り直し、マウントし直す。成功すると epoch が進むので、
   このボリュームに対して発行済みの grant はすべて失効する —— 消えた
   データへの書き込み権限が残らない。
 *
 * **JS タスクから直接呼んではいけない。** 大容量カードでは FAT テーブル
 * だけで数十 MB 書くので、5 秒のコールバック watchdog に確実に轢かれる
 * (docs/filer-storage-design.md §12)。専用タスクへ逃がすこと。 */
esp_err_t fsvol_format(const fsvol_t *v);

/* ---- 仮想パスの解決 --------------------------------------------- */

/* "/sd/DCIM/a.jpg" -> *vol = sd ボリューム, real = "/sd/DCIM/a.jpg"。
 *   ESP_ERR_INVALID_ARG   : 形が不正 (".." / 制御文字 / 長すぎ / 先頭が '/' でない)
 *   ESP_ERR_NOT_FOUND     : その id のボリュームが登録されていない
 *                           (Stamp で "/sd" を開いたときがこれ)
 *   ESP_ERR_INVALID_STATE : 登録済みだが未マウント (カードが入っていない)
 * real が要らない呼び出しでは real=NULL, cap=0 でよい。 */
esp_err_t fsvol_resolve(const char *vpath, const fsvol_t **vol,
                        char *real, size_t cap);

/* エラーを JS へ返す短い英語メッセージに。fsvol_resolve と fs_* 共通。 */
const char *fs_err_str(esp_err_t err);

/* ---- ファイル操作 (どのボリュームでも同じコード) ----------------- */

typedef struct {
    bool     is_dir;
    uint64_t size;
    time_t   mtime;
} fs_stat_t;

typedef struct {
    const char *name;  /* fs_dir_t が持つプールを指す。close で無効になる */
    bool        is_dir;
} fs_entry_t;

typedef struct fs_dir fs_dir_t;

/* ディレクトリを 1 回読み切り、ディレクトリ優先 + 名前順に整列した
   スナップショットを返す (PSRAM)。閲覧中に中身が変わってもイテレータが
   壊れないのと、ページングのたびに readdir し直さずに済むのが狙い。
 *
 * サイズと更新時刻はここでは取らない。FAT のディレクトリ検索は線形なので
 * 全エントリを stat すると O(n^2) になり、写真が数千枚入ったカードで
 * 一覧が止まる。表示する行だけ fs_dir_stat() で後から埋める。 */
esp_err_t   fs_dir_open(const char *vpath, fs_dir_t **out);
int         fs_dir_count(const fs_dir_t *d);
bool        fs_dir_truncated(const fs_dir_t *d);
bool        fs_dir_get(const fs_dir_t *d, int i, fs_entry_t *out);
/* i 番目のサイズ/更新時刻を必要になった時点で取る。 */
esp_err_t   fs_dir_stat(const fs_dir_t *d, int i, fs_stat_t *out);
void        fs_dir_close(fs_dir_t *d);

esp_err_t fs_stat(const char *vpath, fs_stat_t *out);
esp_err_t fs_usage(const char *volume_id, uint64_t *total, uint64_t *freeb);

/* buf に最大 cap バイト読む。*n_read に実バイト数。off がファイル長を
   超えていたら *n_read = 0 で ESP_OK (末尾までは読めた、の意)。 */
esp_err_t fs_read(const char *vpath, uint64_t off, void *buf, size_t cap,
                  size_t *n_read);

esp_err_t fs_write(const char *vpath, const void *buf, size_t len, bool append);
esp_err_t fs_mkdir(const char *vpath);

/* recursive=false ならファイルと空ディレクトリのみ。true なら深さ
   FS_RM_DEPTH_MAX まで潜って消す。 */
esp_err_t fs_remove(const char *vpath, bool recursive);

/* 同一ボリューム内なら rename(2)、またぐならコピーしてから元を消す。
   コピー途中で失敗したら元は残す (中途半端な移動で原本を失わない)。 */
esp_err_t fs_move(const char *from_vpath, const char *to_vpath);

/* progress は NULL 可。done/total はバイト。 */
esp_err_t fs_copy(const char *from_vpath, const char *to_vpath,
                  void (*progress)(void *ctx, uint64_t done, uint64_t total),
                  void *ctx);

#ifdef __cplusplus
}
#endif
