# 依存関係と配布範囲

MoBuVideoPlayerは、MotionBuilder 2026とWindowsが提供する機能を使います。
本プロジェクト自身のライセンスは未選択で、現時点では`LICENSE`を設けていません。
MITなどの利用・改変・再配布許諾を付与したものではありません。
SDK・OS・開発ツールに適用される条件は、本プロジェクトのライセンスとは別です。

## 実行時

- MotionBuilder 2026のOpen Reality APIに動的にリンクします。MotionBuilder本体は別途必要です。
- Windows Media Foundation、WIC、D3D11/DXGI、OpenGL、COM、CNGなどのOS機能を使います。利用できる形式は、そのWindowsにあるデコーダに依存します。
- Visual C++ランタイムは動的リンクです。MotionBuilderが利用するランタイムを前提にしており、ランタイムのインストーラーは同梱しません。
- QuickTime、FFmpeg、FFprobe、VLCのランタイムはプラグインの実行に使いません。

配布物に含めるDLLは、本プロジェクトからビルドした`MoBuVideoPlayer.dll`です。
Autodesk/MicrosoftのSDKヘッダ、インポートライブラリ、製品DLL、サンプルソース、第三者の動画・音声は同梱しません。
SDKの条件からプラグインDLLの配布可否を一律に断定するものではありません。

## 開発と検証

ビルドにはVisual Studio 2022のC++ツール、Windows SDK、CMake、MotionBuilder 2026に付属するOpen Reality SDKを使います。
SDKはビルド環境に別途配置し、`MOBU_ROOT`から参照します。
SDKをリポジトリ、リリース、CIのキャッシュや成果物へコピーしません。

Python、FFmpeg、FFprobeは、デコード比較用のパターン映像・合成音と参照結果を生成する開発テストだけに使います。
これらのツール本体と生成素材は、配布ZIPへ含めません。
新しい依存パッケージや、既存ツールに対する別のライセンスを、この文書によって追加することはありません。

SDKは手元のプラグイン開発環境で使います。CIはGitHub-hostedのWindows環境で、SDKを使わずにネイティブメディア処理と導入・解除PS1を検証します。
配布用のプラグインDLLは、手元でビルドして動作を確認したものを、バージョン・DLLのSHA256・ネイティブソースのfingerprintを記録したJSONとともに用意します。
リリース処理は、その入力とタグの内容を照合し、DLLと導入・解除PS1入りZIPを組み立てます。入力DLLとJSONもリリースの添付物として保持します。

ハッシュの照合は入力内容とソース集合の一致を検査するものです。プラグインDLLの再コンパイルやMotionBuilder上での受入試験をCIで行ったという証明にはなりません。
管理者は手元でのビルド・動作確認に使ったDLLを選び、検証記録とリリース対象を確認してください。

## 一次資料

- [Autodesk MotionBuilder API: SDKは製品リリースに付属](https://aps.autodesk.com/developer/overview/motionbuilder)
- [Autodesk Open Reality SDK 2026 Reference](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/index.html)
- [Microsoft: Media Foundation Source Reader](https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader)
- [Microsoft: WIC pixel formats](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-codec-native-pixel-formats)

SDKに付属する権利表示と、使用する製品・ツールの適用条件も確認してください。この文書は、それらの再配布許諾や法的な保証を与えるものではありません。
