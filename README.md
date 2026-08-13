# wallpaper-mp4 Laptop Edition

`v0.3.0-laptop.1`は通常版`v0.3.0`を基にした、Windows 11ノートPCのハイブリッドGPU構成向け派生版である。統合GPUが内蔵パネルを駆動し、NVIDIA GPUが描画専用になる構成でのDirect3D 9クラッシュと、現行Explorerのデスクトップ層への表示不良を回避する。

H.264 MP4をWindowsデスクトップの壁紙としてループ再生する、軽量な常駐アプリ。

Web表示、プラグイン、インタラクションを持たず、Windows標準のMedia Foundationを利用する。音声は既定でオフ。

## ダウンロード

[Laptop Edition v0.3.0-laptop.1](https://github.com/drydeuterium/wallpaper-mp4/releases/tag/v0.3.0-laptop.1)からWindows x64版ZIPをダウンロードして展開する。インストールは不要。

コード署名は行っていないため、初回起動時にWindows SmartScreenが表示される場合がある。

## 使い方

1. `wallpaper-mp4-laptop.exe`を起動する
2. 「参照...」からH.264 MP4を選ぶ
3. 「壁紙に設定」を押す
4. 音声が必要なら「音声を再生」をオンにする
5. 自動起動するなら「Windowsログイン時に起動」をオンにする
6. 最小化または×でタスクトレイへ格納する

タスクトレイのアイコンをダブルクリックすると設定画面を再表示する。右クリックメニューから動画変更、一時停止、再生、音声切り替え、終了も行える。

最後に選んだMP4と音声設定は次回起動時にも引き継ぐ。設定は `%LOCALAPPDATA%\wallpaper-mp4-laptop\settings.ini` に保存し、動画ファイル自体はコピーしない。通常版とは設定と自動起動登録を分離している。

自動起動は現在のユーザーだけに登録する。Windowsログイン時は設定画面を表示せず、タスクトレイで起動する。無効にする場合はチェックを外す。

ハイブリッドGPU構成では、デスクトップを駆動する統合GPUで再生する。そのため起動時にWindowsのアプリ別グラフィック設定を「省電力」に登録し、設定を変更した初回だけ自動で再起動する。離散GPUが内蔵パネルに接続されていないPCで、MFPlayのDirect3D 9描画を離散GPU側に割り当てるとクラッシュするドライバ構成への回避策である。

現行のWindows 11がraised desktop方式を使う場合は、壁紙ウィンドウを不透明なレイヤード子ウィンドウとして`Progman`内のデスクトップアイコン層とシステム壁紙層の間に配置する。従来方式では、Explorerと同じプロセスが所有する壁紙用`WorkerW`だけを使用する。無関係なプロセスの非表示`WorkerW`へ誤接続して、音声だけが流れる状態になることを防ぐ。

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

生成物は `build\Release\wallpaper-mp4-laptop.exe`。

## 現在の制限

- Windows 10/11 x64向け
- プライマリモニターのみ
- 全画面アプリ、バッテリー、リモートデスクトップに応じた自動停止は未実装
- Explorer再起動後の自動復帰は未検証
- Windowsに動画壁紙の公開APIがないため、Explorerの内部ウィンドウ構造を利用する
