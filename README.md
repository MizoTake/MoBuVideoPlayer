# MoBuVideoPlayer 0.2.1

MotionBuilder 2026 / Windows x64用の動画・音声プラグインです。
Windows標準のMedia Foundationでデコードし、標準Storyの動画・音声クリップ、またはカメラ背景・前景・テクスチャとして利用します。
QuickTime、FFmpeg、VLCなどの再生ランタイムは必要ありません。

## 導入とStoryでの操作

1. 配布ZIPを保管して使うフォルダへ展開し、MotionBuilderを終了します。
2. 展開先の **Install.ps1** を実行します。展開先の plugins を **Settings > Preferences > SDK > Additional Plugins Path** へ登録します。MotionBuilderを通常どおり起動してください。
3. **Open Reality > Tools > Native Video** を開きます。既定の読込先は **Story video + audio (standard clips)** です。
4. **Cache folder...** で保存先を選び、**Import using Windows APIs...** から動画を読み込みます。保存先が空なら、保存済みFBXまたは入力ファイルの隣の NativeVideoMedia を使います。
5. 初回はPNG画像シーケンスとWAVを準備します。ESCを押し続けると、処理の区切りで取り消せます。
6. 生成された標準VideoトラックをViewerへドラッグし、標準のVideo Planeへ割り当てます。音声は標準Audioトラックへ接続します。
7. 標準TransportとStoryで再生・停止・スクラブ、クリップの移動・トリム・速度変更、トラックのMuteを操作します。

映像と音声は別々の標準クリップです。同時編集には両方の選択と標準のグループ化機能を利用してください。
個々のSpeedやトリムを変える場合も、同期を保つには両方の設定を合わせます。
Storyの編集結果をプラグイン独自の再生コントローラーで上書きしません。

現在のタイムライン時刻へ配置します。
MP4の音声遅延・AACの先頭余白と末尾paddingを補正します。
映像が遅れて始まる場合は映像だけを後ろへ配置し、映像より前に始まる音声や映像終了後の音声も保持します。

| 読込先 | 動作 |
| --- | --- |
| Story video + audio | PNGを標準Videoトラック、WAVを標準Audioトラックへ配置 |
| Story audio only | OSで読める音声を標準Audioトラックへ配置。音声のない入力はエラー |
| Current camera background / foreground | 現在のカメラへOS直接デコード映像を接続。音声チェック有効なら標準Audioトラックも作成 |
| Texture only | 標準テクスチャを作成。標準マテリアル等へ割り当て可能 |

Include audioを外すと映像だけを読み込みます。音声のみの読込先では常に音声を読み込みます。
カメラ・テクスチャの NativeVideo.* 設定とStory音声の編集は独立しています。A/Vを一緒に編集する用途にはStory読込を選んでください。
登録・解除用のInstall.ps1 / Uninstall.ps1はWindows標準のWindows PowerShell 5.1で動作し、管理者権限やSDKの導入は不要です。
ビルド・開発用の補助スクリプトは既存のPowerShell 7を使います。通常のプラグイン導入・再生にはPowerShell 7は不要です。
展開先または開発フォルダで次を実行すると、未起動時に設定を保存せず起動できます。
既存のMotionBuilderがある場合はPIDを表示し、新しいプロセスを起動しません。

~~~powershell
./scripts/launch.ps1
~~~

## PS1での登録と解除

ZIPの展開先で次を実行します。この起動に限って実行ポリシーを指定するコマンドで、端末の恒久的な実行ポリシーは変更しません。

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1
~~~

解除するときもMotionBuilderを終了してから、同じ展開先で実行します。

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Uninstall.ps1
~~~

登録はこの展開先を参照するため、利用中はフォルダをその場所に保持してください。
解除は対応する検索パスだけを削除します。展開したファイルとStory用のPNG / WAVは残ります。
別の場所へ移す場合は、移動前に解除してから新しい場所で登録します。
すでに移動した場合は `-PluginDirectory '以前のpluginsフォルダの絶対パス'` を解除時に指定できます。

既存のプラグイン検索パスを保持し、同じパスの再登録・再解除は設定を変更しません。
変更時は設定ファイルの隣に `.bak` を作成します。
`-WhatIf` を付けると、MotionBuilderを起動したまま設定変更の事前確認ができます。

~~~powershell
.\Install.ps1 -WhatIf
.\Uninstall.ps1 -WhatIf
~~~

標準ではWindowsのドキュメントフォルダにある `MB\2026\config\<PC名>.Application.txt` を探します。
初回起動前ならMotionBuilderを一度起動して終了してください。独自の設定フォルダには `-ConfigPath '設定ファイルの絶対パス'` を指定します。
設定が曖昧・破損している場合は自動変更を止めます。
別の登録フォルダに同じDLLがある場合も止め、二重ロードを防ぎます。既存の専用フォルダを解除してから登録し直してください。
他のプラグインと共用する検索パスを解除すると、そのフォルダ内の他のプラグインも読み込まれなくなるため、共用フォルダは手動で管理してください。
設定ファイルはUTF-8を対象とし、セミコロンを含むパスは受け付けません。
手動登録する場合は **Settings > Preferences > SDK > Additional Plugins Path** を使えます。起動時の環境変数 MOTIONBUILDER_PLUGIN_PATH でも指定できます。

## キャッシュと品質

PNGにはOSデコード後のBGRA8画素を可逆圧縮で保存します。H.264等への再圧縮は行いません。
標準Storyは固定FPSの画像シーケンスを使うため、VFRはソースの公称FPSに合わせて時間サンプリングします。
フレームの重複・間引きが起こり得るため、元の可変表示時刻まで完全に可逆ではありません。
CFRは実PTSからFPSの丸めを補正し、フレーム境界で前の画像を選ぶずれを防ぎます。

音声は標準Storyが読み込める整数PCM WAVへ変換します。
16bit・24bit PCMの精度を保持し、8bitは16bitへ拡張します。対応するmono 32bit PCMは32bitを保持します。
Float32や標準ローダーが扱えないstereo 32bitは24bitへ量子化し、ツールの読込結果と NativeVideo.AudioPrecisionReduced に表示します。
この変換は可逆ではありません。元の入力ファイルは変更しません。
元のサンプルレートとチャンネル数は保持します。標準Storyが再生できない構成は読込失敗として通知します。

入力のSHA256、処理方式、キャッシュ改版で識別し、同じ入力の検証済みキャッシュを再利用します。
入力や生成ファイルの内容が変わっていれば無条件には再利用しません。
異なる音声や時刻基準には別のWAV名を使い、標準FBXの音声コピーで同名にならないようにします。
途中失敗・取消時は作成中の領域を削除し、完成済みキャッシュや入力は削除しません。

FBXを移動・共有する際はキャッシュフォルダも一緒に渡してください。
画像シーケンスは多数のファイルと大きな容量を使い、キャッシュは自動削除しません。
Storyのメディアパスは標準FBX機能が保存します。補助情報 NativeVideo.OriginalSource 等は保存時点のFBXが基準で、Save As時の自動更新は行いません。
元の動画がなくても、完成したキャッシュによる標準Story再読込を検証しています。

## カメラ・テクスチャの直接再生

FBVideoMemory経由で標準テクスチャへ映像を渡し、標準Transportの時刻に追従します。
画像シーケンスは作らず、音声を含める場合だけWAVを準備します。
生成されたテクスチャのUser Propertiesで設定します。

| プロパティ | 意味 |
| --- | --- |
| NativeVideo.Source | 動画ファイル。相対パスは現在のFBXが基準 |
| NativeVideo.TimeOffset | 映像を開始するタイムライン時刻 |
| NativeVideo.StartFrame / StopFrame | 0始まりのトリム。StopFrameを含む。-1は末尾 |
| NativeVideo.FrameRate | 0は実PTSの境界。正数は指定FPSで換算 |
| NativeVideo.PlaySpeed / Loop | 1は等速、0は先頭保持、負数は逆再生。Loopは繰返し |
| NativeVideo.Status / Retry | 状態表示と、trueへの変更による再読込 |

直接再生は最初にデコードされた映像をソース時刻0へ正規化し、コンテナの長さまで末尾を保持します。
Story読込は元movieの開始位置と映像・音声それぞれの終端を使います。
設定と接続はFBXへ保存し、直接再生のデコーダとGPUテクスチャは再生成します。
保存済みFBXへの追加は相対パス、未保存シーンは絶対パスです。別フォルダへのSave AsではSourceを調整してください。
実行中の共有は最後の参照まで維持します。設定を持つ元テクスチャを削除し、共有先だけを残したFBXの再読込は受入検証の対象外です。

## パイプライン用API

初期化後のMotionBuilder UIスレッドから呼び出します。専用ツールも同じ処理を使います。

~~~cpp
int MoBuNativeMediaImport(const char* utf8Path, int destination,
    const char* utf8CacheRoot, int includeAudio);
const char* MoBuNativeVideoLastError();
const char* MoBuNativeMediaLastNotice();
int MoBuNativeVideoImport(const char* utf8Path, int destination);
~~~

MediaImportのdestinationは0=背景、1=前景、2=テクスチャ、3=Story動画、4=Story音声です。
includeAudioは0または1、cacheRootは保存先をUTF-8で指定します。
成功1、失敗0。LastErrorは失敗理由、LastNoticeは音声精度変換などの結果です。
文字列は同一スレッドの次回呼出まで有効で、Importの再入は拒否します。
VideoImportは0～2の音声・ディスクキャッシュなしの直接映像読込です。

## 対象と制約

- Windows x64 / MotionBuilder 2026専用。OSがデコードできるローカルファイルを対象とし、H.264 SDR / AACを中心に検証しています。
- GPUデコードと色変換を要求し、利用できない場合はOSのソフトウェア経路へ切り替えます。すべての入力でGPU実稼働を計測した結果ではありません。
- WAVのRIFF上限は約4GiBです。過大な出力は停止します。
- MP4の単一の通常編集と先行empty editに対応します。複数active edit、速度変更edit、未知の映像preroll、壊れた編集表は同期を推測せず停止します。
- HDR、アルファ、回転メタデータ、非正方形ピクセル、途中の解像度変更は対象外です。検出できる条件は停止します。
- 標準MP4ローダーやQuickTimeコーデックの置換ではなく、専用読込で標準Story素材を準備します。DRM、ネットワーク、動画書出しコーデック、埋込タイムコード、macOS / Linuxは対象外です。
- 初回変換には待ち時間と容量が必要です。直接映像再生は描画時にデコード完了を待つため、高解像度・長いGOP・逆スクラブ・複数動画では遅くなる場合があります。
- 深いフォルダ階層の音声キャッシュではWindowsのパス長制約で失敗する場合があります。キャッシュには短い保存先を指定してください。

## ビルドと検証

既存のMotionBuilder 2026 OpenRealitySDK、Visual Studio 2022 C++、Windows SDK、CMake / Ninjaを使います。
新しいSDK・ライブラリは自動ダウンロードしません。MOBU_ROOTか引数でインストール先を指定できます。

~~~powershell
./scripts/build.ps1 -Clean
./scripts/build.ps1 -MotionBuilderRoot 'C:/Program Files/Autodesk/MotionBuilder 2026'
./scripts/launch.ps1 -SmokeTest
./scripts/launch.ps1 -StorySmokeTest
./scripts/package.ps1
~~~

CTestは制御、実デコード、PNG、音声、キャッシュを検証します。
既存Python・FFmpeg・FFprobeでオリジナルの検証素材と参照を生成します。これらは開発専用で、配布物の実行には不要です。
既存ウィンドウで検証する開発用スクリプトは tests/mobu_reuse_checks.py です。Python Editorで実行後、tests/run_reuse_checks.ps1 から操作します。
現在のシーンを初期化せず、専用フォルダへ検証用トラックを追加します。終了時は元の時刻・再生設定へ戻し、検証トラックをミュートして残します。
初期化を前提とする SmokeTest / StorySmokeTest は、既存プロセスがある場合に起動を拒否します。
明示的に新しい検証ウィンドウを1つ開き、終了後も残す場合は `./scripts/launch.ps1 -NewWindow -StorySmokeTest -KeepOpen` を使います。通常の起動は既存ウィンドウを再利用します。
Storyテストは実GL画素、WASAPI loopbackの音声・Mute、FBX保存・再読込も検証します。
実行結果と未確認範囲は[VALIDATION.md](VALIDATION.md)を参照してください。

## GitHubとリリースCI

PRとブランチ更新ではGitHubのWindowsランナーでSDK不要の検証を実行します。
リリースでは、手元で確認したDLLとその検査用JSONをDraft Releaseへ用意し、CIが版数・ハッシュ・ソースの指紋を照合します。
導入・解除PS1入りZIPとSHA256を同じDraftへ添付します。CIでプラグインDLLの再ビルドは行いません。
公開先の準備、入力DLLの用意、タグと手動実行の手順は、ソースリポジトリの `RELEASING.md` を参照してください。
SDKや個人環境の生成物をリポジトリ・配布物へ含めません。依存関係と現在のライセンス状態は [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) に記載しています。

## 設計資料

- [Autodesk: QuickTimeなしでMP4をレビューする方法](https://www.autodesk.com/jp/support/technical/article/caas/sfdcarticles/sfdcarticles/JPN/How-to-review-mp4-video-in-Motionbuilder-without-installing-Quicktime.html)
- [FBStoryClip](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/class_f_b_story_clip.html)、[FBStoryTrack](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/class_f_b_story_track.html)、[FBAudioClip](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/class_f_b_audio_clip.html)
- [FBVideoMemory](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/class_f_b_video_memory.html)、[FBVideoCodecManager](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/class_f_b_video_codec_manager.html)
- [FBConfigFile](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/class_f_b_config_file.html)、[FBSystem.GetPluginPath](https://help.autodesk.com/cloudhelp/2026/ENU/MOBU-CPP-API-REF/class_f_b_system.html)
- [Microsoft Source Reader](https://learn.microsoft.com/en-us/windows/win32/medfound/processing-media-data-with-the-source-reader)、[audio decoding](https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--decoding-audio)、[WIC pixel formats](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-codec-native-pixel-formats)
- [Edit list table](https://developer.apple.com/documentation/quicktime-file-format/edit_list_atom/edit_list_table)

Autodesk / MicrosoftのSDK、サンプルソース、第三者の動画・音声は同梱しません。
