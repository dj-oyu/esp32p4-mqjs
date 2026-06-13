# Audio device capability token 設計

ステータス: **P1 排他 token + P3 overdub mixer 実装済み・実機検証済み
（ブランチ `feat/audio-device-p1`、Tab5/COM8 2026-06-13）。P2 preempt と
プロデューサー移行（tone/WAV/boot/JS）は未了**。

実装メモ:

- `components/audio_device/` 新規。純粋な状態機械
  `audio_device_core.{c,h}`（FreeRTOS 非依存・ロックなし・副作用は
  action list として返すだけ）と、その上の FreeRTOS ラッパ
  `audio_device.{c,h}`（manager task + command queue、core を mutex で保護、
  write ホットパスは token 検証後に `audio_tab5` へ直接）に分離。
- core は `tools/audio_device_test.c` でホスト単体テスト（grant / queue 優先度+
  FIFO / finish dispatch / abort / stale token 拒否 / START→GRANTED 順序 /
  世代再利用 / キュー満杯拒否 / **MIXING の open・join・source 除去・最終 teardown** /
  OVERDUB の EXCLUSIVE 衝突・format 不一致フォールバック）。
  `gcc -fsanitize=address,undefined` で ALL PASS。
- P3 mixer: OVERDUB stream ごとに source ring（16KB PSRAM）、`audio_mix` task が
  各 ring から同一時間幅を取り `int32` 加算→`int16` 飽和→`audio_tab5` master へ。
  EXCLUSIVE 経路は mixer を通らず従来どおり直書き（fast path 維持）。
- `AUDIO_ACT_BACKEND_FLUSH` は現状 `audio_tab5_stop()`（drain）にマップ。
  真の discard API は P2。
- 実機検証（`CONFIG_MQJS_TAB5_AUDIO_DEVICE_SELFTEST`、`audio_device_selftest.c`、
  起動時自己テスト）: S1 EXCLUSIVE+QUEUE（A→B 逐次、slot0 再利用 gen1→2）、
  S2 CANCELABLE（queued D を `audio_request_cancel` で撤回→無音）、
  S3 OVERDUB 旧来逐次、S4 **OVERDUB CHORD**（C5+E5+G5 を t=0 で一斉発音、
  別 slot 0/1/2 gen6/7/8 で並行、~2s 三和音持続、`underruns=0`、実機クリーン確認）。
- **mixer の onset ノイズ対策**: 旧 mixer は `max` フレーム出力＋短いソースを
  ゼロ詰めしていたため、起動充填期に速いリングが毎 tick 削られ頭にノイズ。
  修正＝(1) **min フレーム消費**（全寄与ソースから同数だけ取り、live ソースを
  ゼロ詰めしない）、(2) **per-source prime gate**（各ソースは `PRIME_FRAMES`
  ＝~10ms 充填するまで無音で待機しミックスを律速しない→一斉発音は全員 prime を
  待ってから整列して入る／遅参も既存音を乱さない）、(3) ring 操作はテーブルを
  ロックでスナップショット後 **ロックなし** で実行。実機で一斉発音もクリーン確認。
- 自己テスト（`audio_device_selftest.c` / `CONFIG_MQJS_TAB5_AUDIO_DEVICE_SELFTEST`）は
  検証専用。**既定 off**。通常起動は従来どおり起動 WAV 自動再生
  （`CONFIG_MQJS_TAB5_AUDIO_BOOT_WAV[_AUTOPLAY]`）。
- 未了: P2 preempt（priority + cancel callback + ring 即時破棄）、
  tone / WAV / boot 音楽 / JS binding を `audio_device` 経由へ移行。

本書は、Opus / WAV / tone / boot 音楽など複数の音声プロデューサーを、
Tab5 の単一スピーカーへ安全に合流させる `audio_device` 層を定義する。
物理 I2S / codec / speaker 制御は既存の `audio_tab5` が担い、
`audio_device` はその上で所有権、要求キュー、cancel、将来の overdub を管理する。

- 物理 PCM パイプライン: [audio-pipeline.md](audio-pipeline.md)
- Opus デコーダ統合: [opus-decoder-plan.md](opus-decoder-plan.md)

---

## 1. 目的

現在の `audio_tab5_start/write/stop` は、複数タスクから同時に呼べるが、
「誰が現在の再生を所有しているか」を表現しない。そのため、複数 producer が
同じ PCM ring へ書く、別 producer が sample rate を変更する、他者の
`stop()` が再生を止める、といった競合が起こり得る。

OS レベルの常時稼働ミキサーを最初から導入する代わりに、サウンドデバイスを
**capability token 付きストリームリース**として抽象化する。

token は `net.onReady` の capability token と同じく操作可否の証明だが、
意味は異なる。

- network token: 現在のネットワーク準備完了を証明する
- audio token: 許可された論理ストリームの操作権限を証明する

---

## 2. レイヤと責務

```text
Opus decoder / WAV / tone / boot music / future JS API
                 |
                 v
audio_device
  request queue / policy / token / stream / session / optional mixer
                 |
                 v
audio_tab5
  master PCM ring / writer task / I2S / ES8388 / speaker
```

### `audio_device`

- 再生要求の受付と待機キュー
- stream token の発行、検証、失効
- cancel / preempt / overdub の競合判定
- stream の正常終了、abort、外部 cancel
- overdub 時だけ source ring と mixer を使用

### `audio_tab5`

- 単一 master PCM stream の受け入れ
- I2S / codec / SPK_EN の初期化と再設定
- master ring のバックプレッシャーと drain
- 物理デバイス統計

`audio_tab5` を直接操作できるのは最終的に `audio_device` だけとする。

---

## 3. 中核概念

### Request

まだデバイス操作権限を持たない再生要求。format、policy、callback を持つ。
待機中の request には token を発行しない。

### Stream

許可済みの論理 PCM producer。stream ごとに異なる token を持つ。
排他再生では一つ、mixing session では複数存在できる。

### Session

物理サウンドデバイスを連続して利用する期間。

- `EXCLUSIVE`: 一つの stream が直接 master PCM を供給
- `MIXING`: overdub 許可済み stream 群を mixer で合流

`EXCLUSIVE` session を途中から `MIXING` へ昇格させない。overdub は最初から
overdub 互換として開始された session にのみ参加できる。

### Token

許可済み stream の capability。slot と generation の組で表現する。

```c
typedef struct {
    uint32_t slot;
    uint32_t generation;
} audio_token_t;
```

cancel / abort / release 完了時に generation を更新して失効させる。失効 token
からの write / finish / abort は必ず拒否する。これにより、cancel 後に遅れて
動いた decoder が次の再生を壊さない。

---

## 4. ポリシー

`cancelable` と競合時動作は別の概念として request 時に宣言する。
`finish()` や `release()` 時に宣言してはならない。

```c
typedef enum {
    AUDIO_CONFLICT_QUEUE,
    AUDIO_CONFLICT_PREEMPT,
    AUDIO_CONFLICT_OVERDUB,
} audio_conflict_policy_t;

typedef struct {
    audio_conflict_policy_t conflict;
    bool cancelable;
    uint8_t priority;
} audio_request_policy_t;
```

| 属性 | 意味 |
|---|---|
| `QUEUE` | 現 session 終了まで待機する |
| `PREEMPT` | 条件を満たせば cancelable な現 session を中断する |
| `OVERDUB` | 互換性のある mixing session へ参加する |
| `cancelable` | 他 request による待機撤回・再生中断を許可する |
| `priority` | preempt 可否と待機キュー順を決める |

初期実装は `QUEUE` と `PREEMPT` の排他再生のみを対象とする。`OVERDUB` は
API と状態モデルに予約し、mixer 実装までは互換 session が存在しないものとして
`QUEUE` へフォールバックさせる。

想定 policy:

| 用途 | conflict | cancelable | priority |
|---|---|---:|---:|
| boot 音楽 | `QUEUE` | yes | boot |
| 通常再生 | `QUEUE` | 選択可能 | normal |
| UI 効果音 | 将来 `OVERDUB` | yes | UI |
| 警告音 | `PREEMPT` | no | alert |

---

## 5. 状態遷移

### Request

```text
CREATED -> QUEUED -> GRANTED
    |         |
    +---------+-> CANCELLED
    +------------> REJECTED
```

- `GRANTED` 時にのみ stream と token を生成する。
- cancelable request は `QUEUED -> CANCELLED` へ遷移できる。
- 非 cancelable request は owner 自身の明示撤回を除きキューから除去しない。

### Stream

```text
GRANTED -> STARTING -> PLAYING -> DRAINING -> RELEASED
                |          |
                +----------+-> CANCELLING -> CANCELLED
```

- `finish(token)`: 正常終了。残 PCM を drain 後に token を失効する。
- `abort(token)`: owner 自身による即時停止。残 PCM を破棄して token を失効する。
- 外部 cancel: 管理側が token を先に失効し、producer へ cancel callback を送る。

非 cancelable stream は外部 cancel 遷移を拒否する。owner の `abort(token)` は
許可する。

### Session

```text
IDLE -> EXCLUSIVE -> DRAINING -> IDLE
IDLE -> MIXING    -> DRAINING -> IDLE
```

最後の active stream が `finish` したら session を drain する。cancel / abort
で全 stream が消えた場合は master ring を破棄し、待機 request を直ちに dispatch
する。

---

## 6. 競合判定とキュー

待機 request は単一キューで管理し、基本順序は `priority` 降順、同一 priority
では FIFO とする。

```text
IDLE
  -> request を即時 GRANTED、session 開始

MIXING + 新 request が OVERDUB + format/政策が互換
  -> 即時 GRANTED、現 session へ参加

新 request が PREEMPT
  + 現 session の全 stream が cancelable
  + priority 条件を満たす
  -> 現 stream 群を cancel、新 request を GRANTED

上記以外
  -> QUEUED
```

preempt は session の一部だけを暗黙に残さない。初期設計では、mixing session を
preempt できるのは active stream がすべて cancelable な場合だけとする。

---

## 7. C API 案

### Request

```c
typedef uint32_t audio_request_id_t;

audio_request_id_t audio_request(
    const audio_format_t *format,
    const audio_request_policy_t *policy,
    audio_granted_cb_t on_granted,
    audio_cancelled_cb_t on_cancelled,
    void *arg);

bool audio_request_cancel(audio_request_id_t request);
```

callback は manager task / event loop から非同期に呼ぶ。呼び出し元の stack 上で
同期的に decoder を開始しない。

### Stream

```c
esp_err_t audio_stream_write(
    audio_token_t token,
    const int16_t *pcm,
    size_t frames,
    uint32_t timeout_ms);

esp_err_t audio_stream_finish(audio_token_t token);
esp_err_t audio_stream_abort(audio_token_t token);
bool audio_token_valid(audio_token_t token);
```

`release()` ではなく `finish()` を採用する。単なる所有権返却ではなく、
producer 完了と drain 開始を明示するためである。

### 実装上の不変条件

- token なしで PCM を投入できない
- stream は自身の token 以外を操作できない
- token 失効後の全操作は副作用なしで拒否する
- callback 内で物理デバイス lock を保持しない
- cancel / error / allocation failure の全経路で token を失効させる

---

## 8. Cancelable / 非 cancelable パイプライン

### Cancelable

```text
request(cancelable=true)
  -> QUEUED
  -> GRANTED + token
  -> decoder STARTING/PLAYING
  -> audio_stream_write(token, ...)
  -> finish(token)
  -> master ring drain
  -> token失効
  -> RELEASED
```

外部 preempt 時:

```text
tokenを先に失効
  -> producerへcancel通知
  -> 後続writeを拒否
  -> source/master ringを破棄
  -> CANCELLED
  -> 次requestをGRANTED
```

### 非 cancelable

```text
request(cancelable=false)
  -> GRANTED + token
  -> PLAYING
  -> 別requestはQUEUED
  -> finish(token)
  -> drain
  -> token失効
  -> 次requestをGRANTED
```

外部 preempt は拒否する。owner 自身の `abort(token)` は明示的な異常終了として
許可する。

---

## 9. Overdub と mixer

mix が必要なのは、同じ mixing session への参加を許可された `OVERDUB`
stream 同士だけである。通常の排他再生は mixer を通さない。

```text
OVERDUB stream A -> source ring A --+
OVERDUB stream B -> source ring B --+-> mixer -> master ring -> audio_tab5
OVERDUB stream C -> source ring C --+
```

mixer は `audio_device` 内で `audio_tab5` の直前に置く。

- 各 source ring から同一時間幅の PCM を取得
- stream 単位の gain
- 必要に応じた mono/stereo 変換
- `int32` 加算後に `int16` へ飽和
- cancel / finish 済み source の除去
- 最後の stream 完了後に master ring を drain

初期 overdub 実装では session format を固定し、異なる sample rate の参加は
`QUEUE` へフォールバックする。リサンプラは別フェーズとする。

---

## 10. Boot 音楽

boot 音楽も特別な直接再生経路を持たず、通常の cancelable request とする。

```text
UI/I2C ready event
  -> boot request(QUEUE, cancelable=true)
  -> GRANTED + token
  -> Opus decoder task開始
  -> audio_stream_write(token, ...)
  -> decode完了
  -> finish(token)
  -> drain / token失効
```

- 固定秒数待機は使わない。
- UI/I2C ready event が request 発行条件。
- 待機中または再生中に高優先度のユーザー要求が来たら cancel 可能。
- cancel callback は Opus decoder の abort flag を立てる。
- decoder は各 packet / write 境界で token 有効性または abort flag を確認する。
- 再起動ループを跨ぐ永続 token は存在しない。token generation は boot ごとに初期化する。

---

## 11. 実装フェーズ

### P1: 排他 token

- `audio_device` component と manager task
- `QUEUE`、token 発行・検証・失効
- `write / finish / abort`
- Opus boot 音楽を cancelable request 化
- WAV / tone / JS binding を直接 `audio_tab5` から移行

### P2: Preempt

- priority と `PREEMPT`
- cancel callback
- ring 即時破棄と安全な次 session 開始
- stale token の競合テスト

### P3: Overdub

- `OVERDUB` session と stream ごとの source ring
- mixer task、gain、飽和加算
- 同一 format の overdub
- mixer 無効時の排他 fast path 維持

### P4: 拡張

- 必要ならリサンプラ
- ducking、stream gain、fade
- JS API
- power-state の `KEEP_AWAKE` 判定を session 状態へ移行

---

## 12. 検証項目

- 二つの排他 producer が同じ master ring へ同時書き込みしない
- stale token の write / finish / abort が次 session に影響しない
- cancelable boot Opus を preempt 後、再生タスクが安全に終了する
- 非 cancelable stream が外部 preempt されない
- priority 同値 request が FIFO 順で grant される
- allocation / decode / I2S error 後も次 request が進行する
- mixer 無効時に現在の PCM fast path の性能を劣化させない
- overdub 時だけ mixer が起動し、各 stream の終了が他 stream を停止しない

