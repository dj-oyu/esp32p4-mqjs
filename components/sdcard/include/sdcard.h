#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * "sd" ボリュームを fs_core に登録し、カードが入っていれば併せて
 * マウントする。CONFIG_MQJS_SDCARD が無効なビルドでは何もせず false を
 * 返す (Stamp-P4 がこれ) — 呼び出し側にボードの分岐を書かせないため、
 * 常に呼んでよい形にしてある。
 *
 * 戻り値はマウントできたかどうか。false でも登録は済んでいるので、
 * 後からカードを入れて fsvol_mount() すればよい。
 */
bool sdcard_init(void);

#ifdef __cplusplus
}
#endif
