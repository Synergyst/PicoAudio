# PicoAudio

**Raspberry Pi Pico向けのUSB Audio Class 2.0フルデュプレックス・オーディオファームウェア。PIO/DMAによるI²S再生・キャプチャに対応します。**

PicoAudioは、RP2040およびRP2350マイコンを搭載したRaspberry Pi Pico用の開発プロジェクトです。PicoをUSBステレオオーディオデバイスとして動作させ、USB再生とUSBキャプチャを同時に行い、PIOとDMAを使用してUSBとI²Sの間でオーディオデータを転送します。

> **プロジェクトの状態:** 1台のボードでUSB再生とキャプチャを同時に動作させるテストが完了しています。また、Pico 2 W 1台でI²S送信から受信へのループバックも確認済みです。2台のボードを使用するI²Sブリッジは今後の実装項目です。現在のファームウェアにはI²Sクロックのマスター／スレーブ分離がありません。同じファームウェアを実行している2台のBCLK/LRCLK出力を接続しないでください。

## 主な機能

- USB Audio Class 2.0（UAC2）ステレオ再生・キャプチャ。
- 1台のデバイスでUSB再生とUSBキャプチャを同時実行。
- サンプリング周波数：44.1 kHz、48 kHz。
- 再生フォーマット：
  - 16-bit signed PCM（`S16_LE`）
  - 24-bit packed PCM（`S24_3LE`）
  - 32-bit signed PCM（`S32_LE`）
- キャプチャフォーマット：32-bit signed stereo PCM（`S32_LE`）。
- USB再生フォーマットが16-bitまたはpacked 24-bitの場合も、I²S再生スロットは32-bit。
- PIO/DMAを使用したI²S送信・受信。
- 明示的なフィードバックエンドポイントを使用する非同期USB再生。
- 大きなアイソクロナスパケットで発生した帯域幅エラーを避けるためのFull-Speed USB向けパケットサイズ設定。
- RP2040およびRP2350向けビルド構成。
- 現在Makefileに統合されているPico 1およびPico 2 W向けのビルド／BOOTSEL書き込み補助機能。

## 対応ボード

| ボード | MCU | ビルド設定 | 状態 |
|---|---|---|---|
| Raspberry Pi Pico | RP2040 | `BOARD_NAME=rp2040`、`PICO_BOARD=pico` | ビルド確認済み。再生／キャプチャ経路をテスト済み |
| Raspberry Pi Pico 2 | RP2350 | `BOARD_NAME=rp2350`、`PICO_BOARD=pico2` | ターゲット設定を記載。実機でビルド・動作確認が必要 |
| Raspberry Pi Pico 2 W | RP2350 | `BOARD_NAME=rp2350`、`PICO_BOARD=pico2_w` | ビルド確認済み、実機テスト済み |

Pico 2とPico 2 Wは、どちらもRP2350ファミリーです。PicoAudioはWi-Fiを使用しないため、無印Pico 2に無線機能は必要ありません。無印Pico 2ではPico SDKのボード名`pico2`を、Pico 2 Wでは`pico2_w`を指定します。

現在のMakefileには`build-pico1`と`build-pico2w`が統合されています。無印Pico 2専用のMakefile／デプロイターゲットが追加されるまでは、下記のCMakeコマンドで直接ビルドしてください。Pico SDKでPico 2を指定するボード識別子は`pico2`です。詳細は[Raspberry Pi Pico C/C++ SDKドキュメント](https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html)を参照してください。

## USBオーディオ形式と帯域幅について

USBインターフェースはFull-Speed USB向けに構成されています。アイソクロナスエンドポイントのパケットサイズにもFull-Speedの制限があり、現在の実装では最大392バイト（32-bitステレオサンプル2個で1フレームとした場合、49フレーム分）を使用します。

24-bit再生のAlternate Settingは、3バイトpackedサンプル（`S24_3LE`）として通知します。4バイトのサブスロットで24-bitを通知すると、Linux側で32-bitサンプルコンテナとして扱われ、意図しないAlternate Settingが選択される場合があります。PicoAudioは、対応するUSB再生フォーマットをI²S送信経路の32-bitサンプルスロットへ変換します。

現在有効なサンプリング周波数は44.1 kHzと48 kHzのみです。このFull-Speedファームウェア構成では、より高いサンプリング周波数やHigh-Speed USBには対応していません。

## I²Sピン配置

現在のピン配置は次のとおりです。

| 信号 | GPIO | 現在の1台構成での方向 |
|---|---:|---|
| BCLK | GP20 | I²S送信側から出力。受信側のタイミングにも共有 |
| LRCLK | GP21 | I²S送信側から出力。受信側のタイミングにも共有 |
| I²S送信データ | GP22 | 出力 |
| I²S受信データ | GP26 | 入力 |

### 1台でのループバックテスト

確認済みのループバック接続は次のとおりです。

```text
GP22 (I²S TX DATA)  ─────  GP26 (I²S RX DATA)
GP20 (BCLK)         ─────  共通クロックネット
GP21 (LRCLK)        ─────  共通クロックネット
```

1台構成では送信側がBCLKとLRCLKを生成し、受信側は同じクロックを使用します。GP22からGP26へのデータジャンパーによってデジタルループが完成します。USB再生データをI²Sから出力し、その信号をUSBキャプチャ経由で取り込めます。

Pico 2 W 1台でこのループバックをテスト済みです。左右チャンネルが交互に出力されるテストトーン、およびUSB再生とキャプチャの同時動作を確認しています。

### 2台構成のブリッジ：未実装

2台間でフルデュプレックスI²Sブリッジを実現するには、クロックの役割を分離する必要があります。

- 片方のボードがI²Sクロックマスターとなり、BCLK/LRCLKを出力する必要があります。
- もう片方のボードはそのクロックを入力し、I²S送受信PIOステートマシンをクロックスレーブとして動作させる必要があります。
- 現在のファームウェアにはこのマスター／スレーブ分離がなく、両方のボードでクロック生成型の送信実装が使用されます。

**現在のファームウェアを実行している2台のBCLKまたはLRCLKピンを接続しないでください。** 2つのクロック出力が接続され、電気的な競合が発生する可能性があります。2台接続を行う前に、マスター／スレーブ対応のファームウェア変更が必要です。

将来の2台構成で想定している信号配線は次のとおりです。

```text
Board A TX DATA (GP22)  ─────  Board B RX DATA (GP26)
Board B TX DATA (GP22)  ─────  Board A RX DATA (GP26)
Board A BCLK  (GP20)    ─────  Board B BCLK  (GP20)  [Aがマスター]
Board A LRCLK (GP21)   ─────  Board B LRCLK (GP21) [Aがマスター]
GND                     ─────  GND
```

これは将来の信号配線案であり、現時点で使用可能な接続図ではありません。各ボードはそれぞれ独立してUSB給電してください。VBUS、VSYS、3V3の電源レールを相互接続しないでください。

## ビルド

### 必要なソフトウェア

- Raspberry Pi Pico SDK（開発時の構成はSDK 2.2.0）。
- CMakeおよびGNU Make。
- ARM GNU組み込みツールチェーン（`arm-none-eabi-gcc`）。
- `PICO_SDK_PATH`環境変数（別の方法でPico SDKの場所を設定している場合を除く）。

環境変数の設定例：

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
```

### Makefileに統合されているターゲットのビルド

リポジトリのルートディレクトリで実行します。

```sh
make help
make build
```

現在のMakefileには次のターゲットもあります。

```sh
make build-pico1
make build-pico2w
make status
make deploy
make clean
```

ビルド成果物：

```text
build-pico1/mdac_adc2.uf2
build-pico2w/mdac_adc2.uf2
```

`make deploy`は統合済みターゲットをビルドし、デプロイ用スクリプトがBOOTSELモードとして検出したボードにUF2を書き込みます。実際の動作は、使用しているバージョンの`make help`および`tools/deploy-picos.sh`を確認してください。

### 無印Raspberry Pi Pico 2のビルド

Pico 2ではRP2350ターゲットと`PICO_BOARD=pico2`を使用します。専用のMakefileターゲットが追加されるまでは、次のコマンドで直接ビルドしてください。

```sh
cmake -S . -B build-pico2 \
  -DBOARD_NAME=rp2350 \
  -DPICO_BOARD=pico2 \
  -DPICODAC_I2S_RX_DATA_PIN=26

cmake --build build-pico2 --parallel 7
```

生成されるUF2の想定パス：

```text
build-pico2/mdac_adc2.uf2
```

Pico 2をBOOTSELモードにしてUSB接続し、RP2350のマスストレージボリュームへこのUF2をコピーしてください。無印Pico 2向けのビルド経路は、新しく追加された実機でのコンパイルと動作確認がまだ必要です。

### 手動での書き込み

1. **BOOTSEL**ボタンを押したままPicoをUSB接続します。
2. USBマスストレージボリュームが表示されるまで待ちます。
3. そのボードの種類に合わせてビルドしたUF2をボリュームへコピーします。
4. ボードが再起動し、USBオーディオデバイスとして認識されます。

RP2040用UF2をRP2350ボードに書き込んだり、RP2350用UF2をRP2040ボードに書き込んだりしないでください。Pico 2とPico 2 WはどちらもRP2350ですが、ビルド時にはそれぞれ適切な`PICO_BOARD`設定を使用してください。

## Linuxでのテスト

USBデバイスの認識確認には、次のコマンドが使用できます。

```sh
aplay -l
arecord -l
```

再生テストでは、ALSA/PipeWireアプリケーションでPicoAudioの再生デバイスを選択するか、ALSAが表示したデバイス名を指定して`speaker-test`を実行します。キャプチャテストでは、`arecord -l`や`arecord --dump-hw-params`で確認したデバイスとフォーマットを指定して、短いWAVファイルを録音します。

1台のPicoデバイスで再生とキャプチャを同時にテスト済みです。ALSAのカード番号やデバイス名は、ホスト環境やUSBデバイスの接続順によって変わります。

## リポジトリ構成

| パス | 役割 |
|---|---|
| `main.c` | ファームウェアのエントリーポイント、オーディオサブシステムの初期化 |
| `audio_device.c/.h` | USBオーディオデバイス／クロック処理 |
| `audio_capture.c/.h` | キャプチャ側のオーディオ処理 |
| `i2s.c`、`i2s.pio` | I²S送信実装 |
| `i2s_rx.c/.h`、`i2s_rx.pio` | I²S受信実装 |
| `usb_audio.c` | USB再生処理、サンプル形式変換 |
| `usb_audio_capture.c/.h` | USBキャプチャストリーミング |
| `usb_descriptor.h` | USB Audio/HID複合デバイスのディスクリプター |
| `Makefile` | ビルドおよびデプロイのエントリーポイント |
| `tools/deploy-picos.sh` | BOOTSEL検出とUF2書き込み補助 |

追加の実装メモ：

- [`README-DUPLEX.md`](README-DUPLEX.md)
- [`README-FULL-SPEED-DUPLEX.md`](README-FULL-SPEED-DUPLEX.md)
- [`README-DUAL-BOARD-DEPLOY.md`](README-DUAL-BOARD-DEPLOY.md)

## USB識別子

現在のファームウェアでは、デフォルトで次のUSB識別子を使用します。

- Vendor ID：`0xCAFE`
- Product ID：`0xBABE`

これらは開発用の識別子です。製品として配布したり、一般公開するハードウェアに使用したりする場合は、適切に割り当てられたUSB識別子を使用してください。デフォルト値が自分に割り当てられているとは限りません。

## 原著者への謝辞とライセンス

PicoAudioは、**[td2sk氏による`pico-dac2`](https://github.com/td2sk/pico-dac2)**（Raspberry Pi Pico向けUSB DAC実装）を元に拡張したプロジェクトです。

元の`LICENSE`ファイルは変更せずに保持しており、次の著作権表示も残しています。

```text
Copyright (c) 2025 td2sk
```

ライセンス本文と適用条件については[`LICENSE`](LICENSE)を参照してください。元のプロジェクトおよび原著者をここに明記しています。このREADMEは、PicoAudioで追加した拡張部分をtd2sk氏が作成したと示すものではありません。
