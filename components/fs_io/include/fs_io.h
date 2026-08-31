/*
 * fs_io — ファイル入出力を呼び出し側のタスクから外へ逃がす (仕様 §A.7)。
 *
 * **なぜ要るか。** flash の program/erase 中はキャッシュが落ちて両コアが
 * 止まる。edit_task (core 1, prio 5) が自分で読み書きすると、その停止が
 * そのまま打鍵と描画に出る。だから §A.2 は「edit_task から fs_* を呼ぶな」と
 * 決めていて、その逃がし先がここ。
 *
 * タスクは **core 0, prio 3** —— JS (prio 5) より下。JS のビジーループで
 * I/O が飢える方向に倒す (§E-14 の決定)。逆にすると、重いコピーが JS の
 * 応答を殺す。
 *
 * 呼び出しは任意のタスクから。キューは 8 段で、満杯なら false を返す
 * (待たない。呼び出し側の応答性を人質に取らない)。
 *
 * **コールバックは fs_io タスクの上で動く。** 投げ直すだけにすること ——
 * ここで LVGL を触ったり長い処理をすると、prio 3 のまま UI に割り込む。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSIO_LOAD,      /* from を読む。buf の所有権が cb へ移る */
    FSIO_SAVE,      /* buf を from へ書く (tmp + rename)。buf は cb 後に解放 */
    FSIO_COPY,      /* from -> to */
    FSIO_MOVE,      /* from -> to */
    FSIO_REMOVE,    /* from を消す */
    FSIO_FORMAT,    /* from = ボリューム id */
} fsio_kind_t;

typedef struct {
    fsio_kind_t kind;
    bool        ok;
    esp_err_t   err;
    void       *buf;   /* LOAD のみ。NUL 終端済み。呼び出し側が free する */
    size_t      len;   /* LOAD のみ。NUL を含まない長さ */
} fsio_result_t;

/* fs_io タスク上で呼ばれる。**投げ直すだけ。** */
typedef void (*fsio_cb_t)(void *ctx, const fsio_result_t *r);
/* 同上。長い COPY/FORMAT の途中経過。NULL 可 */
typedef void (*fsio_progress_t)(void *ctx, uint64_t done, uint64_t total);

/*
 * 1 件積む。任意のタスクから呼べる。キューが満杯なら false。
 *
 * from / to は内部に複製されるので、呼び出し側は戻った時点で捨ててよい。
 * SAVE の buf は**所有権が fs_io へ移る** (書き終えたら fs_io が free)。
 * LOAD は PSRAM に確保して読み、その所有権を cb へ渡す。
 */
bool fsio_submit(fsio_kind_t k, const char *from, const char *to,
                 void *buf, size_t len,
                 fsio_cb_t cb, fsio_progress_t prog, void *ctx);

/* タスクを起こす。二度目以降は何もしない。app_main から 1 回。 */
bool fsio_start(void);

#ifdef __cplusplus
}
#endif
