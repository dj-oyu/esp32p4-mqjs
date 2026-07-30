# QR 読み取り性能評価

## 目的

プロビジョニング QR (`MQJSP1`) を Tab5 カメラで読む前に、QR 密度と光学条件の
成立範囲を固定し、端末実装後も同じ条件で性能を比較できるようにする。

## 現在のカメラ処理

- センサー入力: SC2356 (SC202CS driver)、1600x1200 RAW8。
- アプリ取得形式: ISP変換後の1600x1200 RGB565。
- preview/localizer用: 中央 800x600 crop を PPA で 400x300 RGB565 に縮小。
- 既存 EAN-13 telemetry: `camera.status()` の `[pv loc scan ms/f]`。
- 実測済みの支配項は PPA (`pv` 約36ms/frame)。localizerは約11–20ms/frame、
  EAN scanは数ms/frame。

QR は1次元走査線では読めない。実装では既存の400x300中央解析面を8-bit grayscale
へ変換し、`quirc`へ1フレームおきに渡す。現在は`quirc`入力bufferへ直接1回だけ
変換し、反転極性時だけin-place反転を追加する。全面1600x1200 grayscaleは約1.9MBで、
毎frameの変換と探索が重いため採用しない。

システム設定の「QR読み取りテスト」は、読み取った文字列と
`camera.status()` の
`[pv gray mark id dec ms/run markers3/N candidates1 runs... total...ms]`
を端末画面へ表示する。`mark`は3点ファインダーパターン検出時間、`N`は通常極性、
`I`は反転極性を示す。3点が成立しないフレームでは`quirc`を実行しない。
payloadはログとstatusには含めない。

## ISPからYUVを取得する案

### 2026-06-14時点の調査結果

確定事項:

- SC202CS sensor driverが公開するMIPI形式はRAW8/RAW10であり、センサーからYUVを
  直接取得する構成ではない。
- 現在の`cam_tab5`は`/dev/video0`に対して`VIDIOC_S_FMT`で
  `V4L2_PIX_FMT_RGB565`を要求している。
- 現在組み込まれている`esp_video`のISP対応形式表は、RAW8、RGB565、RGB24、
  YUV420、UYVYである。
- `V4L2_PIX_FMT_GREY`の定義と他deviceでの対応コードは存在するが、ISP対応形式表には
  含まれない。そのため、現行componentの`/dev/video0`からGREYを直接取得できるとは
  見込まない。
- `sdkconfig.tab5`はSC202CSの1600x1200 RAW8をdefaultにしている。

推測:

- `/dev/video0`へ`V4L2_PIX_FMT_YUV420`または`V4L2_PIX_FMT_UYVY`を要求すれば、
  sensor RAW8をESP32-P4 ISPでYUVへ変換したframeを取得できる可能性が高い。
- QR decoderは輝度面だけを使うため、YUV420ならY planeをそのままquircへ渡せる。
  現在のRGB565から8-bit grayscaleへの全面変換を省略でき、frame bufferも
  RGB565の3.84MBからYUV420の約2.88MBへ削減できる。
- UYVYでもY byteを直接参照できるためRGB565の重み付きluma変換は不要になるが、
  2 bytes/pixelなのでframe buffer容量はRGB565と同じ。
- ISP変換形式をYUVへ変えるだけでは、QR finder検出やquircの探索時間そのものは
  改善しない。主な改善対象はtelemetryの`gray`とmemory bandwidthである。

### 2026-06-14 実機ログ結果 (COM8)

- `VIDIOC_ENUM_FMT` で `YU12(YUV420)` と `UYVY` は列挙された。
- `VIDIOC_TRY_FMT` は `RGB565` / `YUV420` / `UYVY` / `GREY` の全てで
  `errno=22` になり、実機では対応判定に使えなかった。
- `VIDIOC_S_FMT(diag)` は `YUV420` と `UYVY` で受理された。
  (`1600x1200`, 返却fourcc `YU12`/`UYVY`)
- `VIDIOC_S_FMT(diag)` の `GREY` は
  `CSI can't support format=GREY` で失敗した。
- 結論として、当該構成では
  **YUV420/UYVY は実際に設定可能、GREY は未対応**。

### 既存処理への影響

現在のpipelineは一度`STREAMON`した後、scan間でも閉じずに維持する。以前、
teardown後の再`REQBUFS`/`STREAMON`に失敗しているため、QR scan時だけRGB565とYUVを
切り替える方式は初手にしない。

また、次の処理はRGB565前提である。

- PPAによる400x300解析面とviewfinder生成
- display/viewfinder buffer
- EAN-13 scannerの`luma565`
- barcode localizerのRGB565 pixel fetch

したがってYUV化はQR decoderだけの局所変更ではなく、preview/EAN経路にも影響する。
採用前にPPAが対象YUV形式を入力として扱えるか、YUVからRGB565 previewを十分低コストで
生成できるかを確認する必要がある。

### 次セッションの調査手順

まず永続pipelineの形式を変更せず、起動時診断を追加して実機が列挙する形式を確認する。

2026-06-14 進捗:

- `cam_tab5` に `/dev/video0` open直後の `VIDIOC_ENUM_FMT` /
  `VIDIOC_ENUM_FRAMESIZES` 診断ログを追加済み。
- `VIDIOC_TRY_FMT` で RGB565 / YUV420 / UYVY / GREY を
  1600x1200で試行し、返却形式と `bytesperline` / `sizeimage` を
  記録する診断を追加済み。
- `VIDIOC_S_FMT(diag)` でも YUV420 / UYVY / GREY を試行し、
  実際に `S_FMT` が受理されるかを起動ログで確認できるようにした。
- `VIDIOC_S_FMT` 後の返却 `width` / `height` / `pixelformat` /
  `bytesperline` / `sizeimage` 記録ログを追加済み。
- scan開始後の最初の `DQBUF` で `bytesused` を1回記録するログを追加済み。
- 調査中ビルドでは boot時に `cam_tab5_probe_once()` を呼び、手動スキャン前でも
  V4L2列挙/TRY_FMTログが monitor に出るようにした。

1. `/dev/video0`をopenした直後、`VIDIOC_ENUM_FMT`をindexが尽きるまで実行し、
   fourccとdescriptionをログへ出す。
2. 各形式について`VIDIOC_ENUM_FRAMESIZES`で1600x1200と1280x720の対応を確認する。
3. 通常動作では従来通りRGB565を`VIDIOC_S_FMT`し、driverが返したwidth、height、
   pixelformat、bytesperline、sizeimageを記録する。
4. 別の診断buildでYUV420を`VIDIOC_S_FMT`し、返却形式と1 frameの
   `bytesused`を確認する。次点でUYVYを試す。
5. YUV420 frameの先頭Y planeをquirc入力として使い、同じQRと表示条件で
   `gray`、`mark`、`id`、`dec`、`total`をRGB565版と比較する。
6. previewを無効化したQR専用診断で成立を確認してから、preview/EANとの共存方式を
   決める。

診断buildではpayloadやframe内容をログへ出さない。形式切り替え後にscanを終了して
同一boot内でRGB565へ戻す試験も行わない。永続pipelineを壊した場合はrebootで復帰する。

### 判断基準

- YUV420が1600x1200で安定してDQBUF/QBUFできる。
- QRの`gray`時間がほぼゼロになり、`total`または読み取り成功までの時間が明確に改善する。
- ISP/PPA errorがRGB565版より増えない。
- previewとEANを維持するための追加変換コストが、YUV化で削減したコストを上回らない。

YUV420が使えてもpreview/EAN共存のコストが高い場合、通常pipelineはRGB565のままとし、
現在のmarker prefilterとRGB565-to-gray変換の最適化を継続する。

### 現時点の方針 (2026-06-14)

- 本流は `RGB565` を維持する。
  - preview/LVGL表示とEAN/localizerがRGB565前提で結合しており、
    YUV化はQR局所最適化ではなくパイプライン再設計になるため。
- `YUV420` は将来候補として保持する。
  - QRはY plane直結で有利だが、preview/EANをどう共存させるかを先に設計する。
- `GREY` は採用対象から外す (現行CSI/ISPで未対応)。

### 2026-06-14 実機比較実験 (COM8 monitor)

`camera.status()` 依存を避けるため、QR実行ごとに monitor へ次のテレメトリを直接出力する
計測を追加した。

```text
qr run gray=<ms> y2r=<ms> mark=<ms> id=<ms> dec=<ms> markers=<n>/<N|I> candidates=<n> found=<Y|N>
```

- `gray`: 現行パイプラインの RGB565 -> gray (quirc入力) 変換時間
- `y2r`: 同解像度での YUV420 -> RGB565 相当変換の疑似計測時間
- `mark`/`id`/`dec`: 既存QR処理の内訳時間

monitorログの代表値:

- `gray=21..32ms`
- `y2r=32..45ms`
- `mark=8..14ms`
- `candidates=0`, `found=N` が継続

結論:

- 現状実装・同条件では **RGB565 -> gray の方が YUV420 -> RGB565 より軽い**
  （概ね 10ms 前後有利）。
- 今回の試行では候補検出段階に到達しておらず、decode比較 (`id`/`dec`) まで進んでいない。

## Decoder候補

端末側の第一候補は `quirc`。

- dependencyなしのC実装。
- 入力は8-bit grayscale、1 byte/pixel。
- decoder objectと画像bufferを再利用できる。
- 回転、斜視、複数QRに対応。
- 400x300で画像bufferは約120KB。decoder bookkeepingを加えても現構成で扱える範囲。
- `QUIRC_FLOAT_TYPE=float`を使い、P4のsingle precision FPUに合わせて測定する。

`zxing-cpp`はホスト基準decoderとして使う。認識限界の比較には有用だが、初期の端末
組み込み候補には大きすぎる。

## ホスト光学ベンチ

[`tools/qr_read_bench.py`](../../tools/qr_read_bench.py) は実際の`MQJSP1` payloadを生成し、
400x300画像へ次の劣化を加えて成功率とhost decode時間を測る。

- QR表示辺長とpixel/module
- 回転
- ランダムな遠近変形
- Gaussian blur
- 低contrast

```powershell
uv run tools/qr_read_bench.py `
  --csv C:\tmp\qr-bench.csv `
  --markdown C:\tmp\qr-bench.md
```

host時間はESP32-P4時間ではない。ここで見るべき値は成功率と、必要なpixel/moduleの
下限。QR画像を印刷・別画面表示する際のガイドへ使う。

## 2026-06-14 ホスト結果

実際の`MQJSP1` payloadで400x300 canvasを評価した。

| Payload | QR modules | 条件 | 100%成功した最小辺長 | pixel/module |
|---|---:|---|---:|---:|
| Wi-Fi only | 49 | 回転0/15/30°、遠近0/8%、blur 0/1px | 160px | 2.81 |
| Wi-Fi + Tailscale | 65 | 回転0/15/30°、遠近0/8%、blur 0/1px | 220px | 3.01 |

穏当条件の全480ケースでは成功率74.4%。combined payloadは辺200pxで86.7%、
辺220pxで100%。Wi-Fi-onlyは辺160px以上で100%だった。

強い劣化条件（遠近12%、blur 2px、contrast 0.6を含む）では全864ケースの成功率は
33.2%。combined payloadは辺220pxでも62.0%であり、blur/低contrast時は400x300面だけ
では余裕がない。

結論:

- 生成・表示ガイドは最低3 pixel/moduleとする。
- Wi-Fi-only QRは400x300解析面で十分な余裕がある。
- combined QRは読めるが、画面高300pxに対して辺220px以上を占める必要がある。
- combined QRが1秒読めない時は中央800x600 high-resolution passを使う。
- auth keyが長くQR modulesが増えた場合はWi-Fi/Tailscaleを別QRへ分割する。

ホストの`zxing-cpp` decode時間は成功caseで概ね0.3ms、失敗探索で約2msだった。
これはESP32-P4/quircの処理時間を示さないため、実機合否には使わない。

## 実機ベンチ計画

COM8が空いた後、quircを組み込んで次を`camera.status()`へ追加する。

```text
[qr gray identify decode ms/f found/N]
```

計測を分離する。

| 指標 | 範囲 |
|---|---|
| `gray` | RGB565 400x300からquirc bufferへのluma変換 |
| `identify` | `quirc_end()`までのfinder/region認識 |
| `decode` | 全候補の`quirc_decode()`合計 |
| `ms/f` | QR処理全体 |
| `found/N` | 成功frame数 / 処理frame数 |

### 合格基準

- QR処理 p50 が 50ms/frame 以下。
- QR処理を毎frame実行しない構成でも、静止QRを1秒以内に読む。
- 400x300上でcombined payloadを辺160px以上に提示した時、正対・通常照明で成功率95%以上。
- QRがない時にUI previewとMQJS schedulerを長時間stallしない。
- 読み取ったpayloadやcamera frameをログ・永続領域へ出さない。

### 最初に試すscheduler

- previewは既存通り毎frame。
- QR decoderは2frameに1回、400x300解析面で実行。
- decode成功時だけMQJSP1 envelopeを検証する。
- 1秒読めない場合のみ、中央800x600 grayscaleの高解像度passを1回試す。

この構成なら、QRがない通常frameの負荷を抑えながら、密度の高いcombined payloadへ
高解像度fallbackを使える。

## 参考

- quirc README: <https://github.com/dlbeer/quirc>
- zxing-cpp: <https://github.com/zxing-cpp/zxing-cpp>
