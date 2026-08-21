# CDJ-3DS

rekordbox の Device Library を読み込み、Nintendo 3DS/3DS LL を 1 Deck の DJ プレーヤーとして使う Homebrew アプリです。上画面はデッキ情報と全体波形、下画面はランタイム波形とタッチ操作を表示します。

> 非公式のファンメイド・プロトタイプです。Pioneer DJ / rekordbox の公式製品ではありません。

## 対応環境

- 改造済み Nintendo 3DS / 3DS LL（Luma3DS + FBI）
- rekordbox から SD カードへエクスポートした Device Library
- MP3、AAC / M4A（AAC-LC）の楽曲
- 3DS の SD カード内にある `PIONEER` フォルダ

`3ds_one_deck_himem.cia` は、旧3DS/3DS LL でも通常の Homebrew Launcher より多いメモリを使うための CIA 版です。3DSX 版も同梱しています。

## 使い方

1. rekordbox で楽曲を SD カードへエクスポートします。
2. SD カードにアプリを配置します。

   ```text
   SD:/3ds/3ds_one_deck/3ds_one_deck.3dsx
   SD:/3ds/3ds_one_deck/cache/library.rbd
   ```

3. より多いメモリで使う場合は FBI から `3ds_one_deck_himem.cia` を **Install CIA** でインストールし、HOME メニューの CDJ-3DS を起動します。
4. `X` でブラウザを開き、十字キー上下で曲を選択して `A` でロードします。

アプリはエクスポート済みの Device Library からタイトル、作曲者、BPM、キー、アートワーク、波形、ビートグリッド、Memory Cue、Hot Cue を読み込みます。曲を追加・再エクスポートした際は `cache/library.rbd` を更新してください。

## AAC / M4A をMP3化するPCツール

`tools/convert_device_library_mp3.py` は、Device Library の元ファイルを変更せずに AAC / M4A / MP4 を 44.1 kHz ステレオ CBR MP3 へ変換します。変換後のMP3と更新済み `library.rbd` はCDJ-3DS専用キャッシュに置かれるため、曲名・アートワーク・グリッド・Cueは元のrekordboxエクスポートをそのまま利用できます。

FFmpegをインストールしたPCで、SDカードを挿して実行します。

```powershell
py tools\convert_device_library_mp3.py F:\
```

標準は320 kbpsです。容量を優先する場合は `--bitrate 256k`、FFmpegをPATHへ追加していない場合は `--ffmpeg C:\path\to\ffmpeg.exe` を付けます。

```powershell
py tools\convert_device_library_mp3.py F:\ --bitrate 256k
```

出力先は `SD:/3ds/3ds_one_deck/cache/audio/performance/` です。PIONEERフォルダと元のM4A/AACは一切置き換えません。

## 基本操作

| 操作 | 内容 |
| --- | --- |
| `B` | 再生 / 一時停止 |
| `A`（停止中） | 現在位置を Cue に設定して、押している間だけ再生 |
| `A`（再生中） | 音を止めて Cue 位置へ戻る |
| `X` | ブラウザを開く / 閉じる |
| 十字キー `←` / `→` | 上画面のメニューを切替 |
| 十字キー `↑` / `↓` | ランタイム波形の拡大率を変更 |
| `Y` + 十字キー `←` / `→` | Beat Jump（現在の設定拍数で戻る / 進む） |
| `R` + 十字キー `←` / `→`（停止中） | 前 / 次の Memory Cue へ移動 |
| スライドパッド左右 | 一時的なピッチベンド（再生中） |
| 下画面の波形をドラッグ | スクラッチ / 再生位置の移動 |
| 下画面右上の Tempo Range | `±6` → `±10` → `±16` → `WIDE` を切替 |
| 下画面のテンポフェーダー | テンポを変更 |

## 下画面メニュー

上画面のメニューを `←` / `→` で選び、下画面をタッチして操作します。

- **HOT CUE** — 8 パッド。空きパッドは現在位置に Hot Cue を登録し、登録済みパッドは呼び出します。`Y` を押しながらタップすると削除します。
- **MEMORY CUE** — Memory Cue の登録・削除。
- **BEAT LOOP** — `LOOP IN`、`LOOP OUT`、`4 BEATS`、`8 BEATS`、`EXIT`。ループ中は `4 BEATS` が `×1/2`、`8 BEATS` が `×2` になります。
- **BEAT JUMP** — 1 / 2 / 4 / 8 拍の前後ジャンプ。
- **SETTING** — Quantize、Beat Jump 拍数、Auto Cue、A.Hot Cue、Master Tempo などを設定します。

Quantize を有効にすると、Cue と Loop の境界は選択したビート単位へ丸められます。

## 注意点

- 音声は 3DS のヘッドホン端子からステレオで出力されます。ライン入力へ接続する場合は、音量を下げてから接続してください。
- AAC / M4A は楽曲のエンコード形式によってデコードできないことがあります。再生できない曲は MP3 への変換を推奨します。
- Hot Cue、Loop、Beat Jump は音声処理を最優先に実装していますが、旧3DS/3DS LL では高ビットレート AAC/M4A・大きなテンポ変更時に処理負荷が高くなることがあります。
- アプリで変更した Cue 情報は `SD:/3ds/3ds_one_deck/cache/cue-overrides.rbd` に保存されます。rekordbox の元データベースは直接変更しません。

## ビルド

devkitPro / devkitARM が必要です。devkitPro MSYS シェルで実行します。

```sh
make          # 3DSX を生成
make cia      # 96 MB モードの CIA を生成
```

生成物:

- `3ds_one_deck.3dsx`
- `3ds_one_deck_himem.cia`

## ライセンスと権利表記

Pioneer DJ、rekordbox は各権利者の商標です。本プロジェクトは教育・実験目的の非公式ソフトウェアです。楽曲ファイルおよび rekordbox エクスポートデータはリポジトリに含めません。
