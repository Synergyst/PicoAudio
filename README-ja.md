# PicoAudio

**Raspberry Pi Pico向けのUSB Audio Class 2.0フルデュプレックス音声ファームウェアです。PIO/DMAによるI²S再生・キャプチャに対応します。**

PicoAudioは、各PicoボードをステレオUSBオーディオデバイスとして動作させ、再生と録音を同時に行います。USB PCMデータはRP2040/RP2350のPIOおよびDMAを使ってデジタルI²Sへ送受信します。この経路ではアナログADC/DACを使用しません。

## 現在の状態

- 1台のPicoでUSB再生とキャプチャを同時に動作させるテストを実施済みです。
- Pico 2 Wで、1台のボード内のI²S TX→RXループバックをテスト済みです。
- I²Sクロックマスター用とクロックスレーブ用のファームウェアを実装しました。
- 2台を接続したデータ経路は、これから実機で検証する段階です。検証が完了するまでは、2台間ブリッジが動作確認済みとはみなさないでください。

## 主な機能

- USB Audio Class 2.0（UAC2）ステレオ再生・キャプチャ
- 各ボードでのUSB再生とキャプチャの同時動作
- 44.1 kHz / 48 kHz
- 再生形式: S16_LE、packed S24_3LE、S32_LE
- キャプチャ形式: ステレオ S32_LE
- フルデュプレックス経路での32-bit I²Sスロット
- PIO/DMAによるI²S送受信
- 明示的なfeedback endpointを使用する非同期USB再生
- コンパイル時に選択するI²Sクロックマスター／スレーブ
- RP2040およびRP2350向けビルド

## 対応ボード

| ボード | MCU | Pico SDK名 | 役割／状態 |
|---|---|---|---|
| Raspberry Pi Pico | RP2040 | `pico` | マスター用ビルド。従来のビルド確認済み |
| Raspberry Pi Pico 2 | RP2350 | `pico2` | 新しいスレーブ用ターゲット。実機でのビルド・動作確認が必要 |
| Raspberry Pi Pico 2 W | RP2350 | `pico2_w` | マスター用ターゲット。従来の単体動作はビルド・実機確認済み |

PicoAudioはWi-Fiを使用しません。Pico 2 WのオンボードLEDはCYW43経由で接続されるため、通常のPIO GPIO LEDとしては使用しません。

## I²Sピン割り当て

| 信号 | GPIO | マスター | スレーブ |
|---|---:|---|---|
| BCLK | GP20 | 出力 | 入力 |
| LRCLK | GP21 | 出力 | 入力 |
| TX DATA | GP22 | 出力 | 出力 |
| RX DATA | GP26 | 入力 | 入力 |

BCLKとLRCLKを駆動するのはマスターのみです。スレーブ側の送信PIOは外部クロックに同期してDATAを出力し、クロックピンは駆動しません。

## 2台分のファームウェアをビルド

Pico SDK 2.2.0を使用し、`PICO_SDK_PATH`を設定してからリポジトリのルートで実行します。

```sh
make build-bridge
```

生成されるファイル:

| 物理ボード | 役割 | 出力ファイル |
|---|---|---|
| Pico 2 W | I²Sクロックマスター | `build-pico2w/mdac_adc2.uf2` |
| Pico 2（非W） | I²Sクロックスレーブ | `build-pico2-slave/mdac_adc2.uf2` |

マスター用UF2はPico 2 Wへ、スレーブ用UF2は非WのPico 2へ書き込んでください。2つのUF2は入れ替えできません。役割を指定できる`make deploy`を使用できます。Pico 1は`RPI-RP2`ラベルで自動判別します。Pico 2とPico 2 Wはどちらも`RP2350`ラベルになるため、デプロイ時に物理ボードの種類を選択します。

ボードと役割を指定してビルドできます:

```sh
make build BOARD=pico1 ROLE=master
make build BOARD=pico2 ROLE=slave
make build BOARD=pico2w ROLE=master
```

`make build-pico1`、`make build-pico2`、`make build-pico2w`も使用できます（既定の役割はmasterです）。`ROLE=slave`を指定するとスレーブ用イメージを生成します。`BOARD`を指定せずに`make build`を実行すると、Pico 2 W master + Pico 2 slaveの標準ブリッジ構成をビルドします。

デプロイは次のように実行します:

```sh
make deploy
make deploy BOARD=pico2w ROLE=master
make deploy BOARD=pico2 ROLE=slave
```

Pico 1は`RPI-RP2`ラベルから自動判別します。Pico 2とPico 2 Wは同じ`RP2350`ラベルを使用するため、モデルが自動判別できない場合は選択を求めます。デプロイはroot権限で実行し、1回の実行で選択したBOOTSELボード1台に書き込みます。

## 2台の配線

適切な役割のUF2を書き込んだ後、次のように接続します。

```text
Pico 2 W (MASTER)                  Pico 2 (SLAVE)

GP20  BCLK OUT  -----------------> GP20  BCLK IN
GP21  LRCLK OUT -----------------> GP21  LRCLK IN
GP22  TX DATA    ----------------> GP26  RX DATA
GP26  RX DATA    <---------------- GP22  TX DATA
GND              ----------------- GND
```

DATAは交差接続です。GP22同士、またはGP26同士を接続しないでください。GNDは共通にしますが、電源はそれぞれのUSBから個別に供給してください。**VBUS、VSYS、3V3を2台間で接続しないでください。**

マスターはUSB再生ストリームが動作している間だけBCLK/LRCLKを生成します。マスターの再生が停止すると共有I²Sクロックも停止し、スレーブ送信と両方のキャプチャに新しいサンプルが届かなくなります。両方のボードで同じサンプルレート（44.1 kHzまたは48 kHz）を選択してください。

詳しい書き込み手順と方向別テストは[README-DUAL-BOARD-ROLES.md](README-DUAL-BOARD-ROLES.md)を参照してください。

## USB形式と帯域幅

USBはFull Speedです。現在のアイソクロナス転送バッファー上限は392バイト（32-bitステレオフレーム換算で最大49フレーム）です。24-bit再生は3バイトのpacked `S24_3LE`として公開し、I²S経路で使用する32-bitワードへ変換します。現在有効なサンプルレートは44.1 kHzと48 kHzです。

## 帰属表示とライセンス

PicoAudioは、**[td2sk氏の`pico-dac2`](https://github.com/td2sk/pico-dac2)**を基に拡張したプロジェクトです。元の`LICENSE`ファイルを保持しており、`Copyright (c) 2025 td2sk`の著作権表示も残しています。完全なライセンス条項は[LICENSE](LICENSE)を参照してください。PicoAudioで追加した機能を元の作者が開発したと示すものではありません。
