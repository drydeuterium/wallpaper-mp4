# wallpaper-mp4

H.264 MP4だけをWindowsデスクトップの背面で再生する、最小構成の常駐アプリ。

Web表示、プラグイン、インタラクション、音声再生を持たず、Windows標準のMedia Foundationを利用する。外部プレイヤーや追加ランタイムは同梱しない。

## 現在の機能

- H.264映像を含むローカルMP4の再生
- 音声ストリームの無効化
- 無限ループ
- プライマリモニターへの表示
- アスペクト比を維持したフィット表示
- 動画選択、適用、一時停止、終了を行う軽量なWin32設定画面
- 設定画面の最小化または×でタスクトレイへ格納
- タスクトレイからMP4の変更、一時停止、再生、終了
- 最後に選んだMP4を次回起動時に再利用

設定は `%LOCALAPPDATA%\wallpaper-mp4\settings.ini` に保存する。動画ファイル自体はコピーしない。

## ビルド

必要なもの：

- Windows 10またはWindows 11
- Visual Studio Build Tools
- 「C++によるデスクトップ開発」
- Windows 10/11 SDK
- 「Windows用C++ CMakeツール」

PowerShellで実行する。

```powershell
.\scripts\build.ps1 -Configuration Release
```

生成物：

```text
build\Release\wallpaper-mp4.exe
```

## 実行

MP4を引数に渡すか、引数なしで設定画面を開く。

```powershell
.\build\Release\wallpaper-mp4.exe "C:\Videos\wallpaper.mp4"
```

設定画面の「参照...」でMP4を選び、「壁紙に設定」を押すと再生を開始する。前回選んだMP4が存在する場合は、次回起動時に自動再生する。

最小化または×で設定画面だけを隠し、壁紙再生は継続する。タスクトレイアイコンをダブルクリックすると設定画面を再表示し、右クリックすると操作メニューを開く。アプリ自体を止める場合は「終了」ボタンまたはトレイメニューの「終了」を使う。

## H.264への変換例

音声を削除し、8-bit 4:2:0、30 fpsのH.264 MP4に変換する例：

```powershell
ffmpeg -i input.mp4 -an -vf fps=30 -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -movflags +faststart output.mp4
```

解像度はディスプレイと同じにしておくと、再生時の拡大縮小コストを避けやすい。

## 現時点の制限

- 複数モニターの個別表示には未対応
- 全画面アプリ、バッテリー駆動、リモートデスクトップを検出した自動停止は未実装
- Explorer再起動後の自動復帰は未検証。復帰しない場合はアプリの再起動が必要
- Windowsには動画壁紙の公開APIがないため、Explorerの`WorkerW`ウィンドウを利用する。この挙動はWindows更新で変化する可能性がある
- 初期実装はMedia FoundationのMFPlayを利用する。GPUのVideo Decode使用は確認済みだが、MFPlayはMicrosoftがレガシー扱いしているため、将来の再生API更新候補とする

まず壁紙配置とH.264ループ再生を検証し、その後に計測結果を基準として再生経路や自動停止を最適化する。

## 検証済み環境

2026-08-06時点で、Windows 11 Home build 26200、GeForce RTX 5060 Ti、2560 x 1440のプライマリディスプレイ上で確認した。

- ReleaseビルドがMSVCの警告なしで成功
- 2秒のH.264テスト動画が5秒を超えて再生され、ループを継続
- 動画の上にデスクトップアイコンとタスクバーが表示されることを画面キャプチャで確認
- 設定画面の最小化・×によるトレイ格納、トレイからの復帰、一時停止・再開、終了を確認
- 一時停止中の2秒間のプロセスCPU時間が0.00秒であることを確認
- WindowsのGPU Engineカウンターで`VideoDecode`の使用を確認
- 1920 x 1080、約58 fpsのH.264実動画では、5秒間の計測でVideo Decode平均2.34%、3D平均1.95%、CPU時間0.31秒、ワーキングセット約79.7 MB

GPU使用率とメモリ値はGeForce RTX 5060 Ti上での単発測定であり、Wallpaper Engineとの比較ベンチマークではない。
