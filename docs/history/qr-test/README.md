# 実機QR読み取り試験

## 試験対象

- システムアプリ起動:
  ランチャーの「デバイス設定」から、ファームウェア組み込みの設定画面を開けること。
- QR結果:
  「QR読み取りテスト」で、読み取った文字列が端末画面に表示されること。
- QR性能:
  結果末尾の `camera.status()` で次を確認する。

```text
[pv36 gray2 mark1 id8 dec1 ms/run markers3/N candidates1 runs3 total420ms]
```

- `pv`: 1フレームあたりの中央crop/preview処理時間
- `gray`: 1回あたりのRGB565からgrayscaleへの変換時間
- `mark`: 1回あたりの3点ファインダーパターン検出時間
- `markers3/N`: 検出マーカー数と極性。`N`は通常、`I`は反転
- `id`: 1回あたりのQR候補検出時間
- `dec`: 1回あたりのQR復号時間
- `runs`: 読み取り成功までのdecoder実行回数
- `total`: スキャン開始から成功までの総時間

QR payloadは端末画面にだけ一時表示され、ログ、status、Vaultには保存されない。

## 試験素材

- `wifi-short.png`: 125-byte MQJSP1 Wi-Fi payload。
- `combined-long.png`: 247-byte MQJSP1 Wi-Fi + Tailscale payload。
- `combined-long-dark.png`: 同じpayloadの黒背景版。画面撮影時の白飛び対策用。
- `corpus/`: 400x300画像内でQR辺長を140/160/180/200/220pxに変えた比較素材。

全て試験用ダミー資格情報であり、実ネットワークには接続しない。

## 手順

1. PC画面へ `combined-long-dark.png` を等倍表示する。画面輝度は30%程度から始める。
2. 端末ランチャーで「デバイス設定」を開く。
3. 「QR読み取りテスト」から読み取りを開始する。
4. 読み取り結果が `MQJSP1:` で始まることと、性能値を記録する。
5. `corpus/` の220pxから140pxへ順に切り替え、読める最小サイズを確認する。
6. 同じ画像で距離、傾き、照明を変え、`runs` と `total` の変化を確認する。

400x300解析面では、まず3px/module前後を安定動作目標とする。
