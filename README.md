# wallpaper-mp4

H.264 MP4をWindowsデスクトップの壁紙としてループ再生する、軽量な常駐アプリ。

Web表示、プラグイン、インタラクションを持たず、Windows標準のMedia Foundationを利用する。音声は既定でオフ。

## ダウンロード

[最新のRelease](https://github.com/drydeuterium/wallpaper-mp4/releases/latest)からWindows x64版ZIPをダウンロードして展開する。インストールは不要。

NVIDIA/AMDまたはIntel/NVIDIAのハイブリッドGPUを搭載したノートPCで、通常版が再生時にクラッシュする、または音声だけ流れて映像が壁紙に表示されない場合は、[Laptop Edition](https://github.com/drydeuterium/wallpaper-mp4/releases/tag/v0.3.0-laptop.1)を使用する。

コード署名は行っていないため、初回起動時にWindows SmartScreenが表示される場合がある。

## 使い方

1. `wallpaper-mp4.exe`を起動する
2. 「参照...」からH.264 MP4を選ぶ
3. 「壁紙に設定」を押す
4. 音声が必要なら「音声を再生」をオンにする
5. 自動起動するなら「Windowsログイン時に起動」をオンにする
6. 最小化または×でタスクトレイへ格納する

タスクトレイのアイコンをダブルクリックすると設定画面を再表示する。右クリックメニューから動画変更、一時停止、再生、音声切り替え、終了も行える。

最後に選んだMP4と音声設定は次回起動時にも引き継ぐ。設定は `%LOCALAPPDATA%\wallpaper-mp4\settings.ini` に保存し、動画ファイル自体はコピーしない。

自動起動は現在のユーザーだけに登録する。Windowsログイン時は設定画面を表示せず、タスクトレイで起動する。無効にする場合はチェックを外す。

## 対応動画

- コンテナ：MP4
- 映像：H.264
- 推奨：8-bit、4:2:0、ディスプレイと同じ解像度、30 fps
- 音声：オン／オフ切り替え可能。対応形式はWindows Media Foundationに依存

FFmpegでの変換例：

```powershell
ffmpeg -i input.mp4 -vf fps=30 -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart output.mp4
```

## ビルド

Visual Studio Build Toolsの「C++によるデスクトップ開発」と「Windows用C++ CMakeツール」をインストールし、PowerShellで実行する。

```powershell
.\scripts\build.ps1 -Configuration Release
```

生成物は `build\Release\wallpaper-mp4.exe`。

## 現在の制限

- Windows 10/11 x64向け
- プライマリモニターのみ
- 全画面アプリ、バッテリー、リモートデスクトップに応じた自動停止は未実装
- Explorer再起動後の自動復帰は未検証
- Windowsに動画壁紙の公開APIがないため、Explorerの内部ウィンドウ構造を利用する
