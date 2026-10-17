# 検証記録 0.2.1

実施日：2026-10-17（日本時間）。

## 対象環境と成果物

- Windows 11 Pro x64、OS build 26300。
- MotionBuilder 2026 / 同梱OpenRealitySDK。実ホストはこのPCのTrial版。
- Visual Studio 2022、MSVC 19.44、Windows SDK 10.0.26100.0、既存のCMake / NinjaでRelease x64を生成。
- NVIDIA RTX 4070 / Tesla P100搭載環境。検証素材のH.264生成には既存FFmpegのNVENCを使用。
- 新しいSDK・コーデック・再生ランタイムの導入は実施していません。
- 実ホスト検証に用いたDLL：310272 bytes、SHA256 `fb3641ce9c2e2a31533f00150c125bd5ae359f530aca60cdd8f47f377de27502`。

## 自動テスト

0.2.0のDLLに対して `./scripts/build.ps1` を実行し、Releaseビルドとネイティブ処理のCTestの6スイートすべてが成功しました（120.49秒）。

| スイート | 結果 | 主な検証内容 |
| --- | --- | --- |
| playback | 28項目成功 | オフセット、速度、逆再生、ループ、トリム、境界 |
| sequence unit | 67項目成功 | FPS、フレーム数、VFRサンプリング、取消・出力上限 |
| decoder integration | 7素材・73項目成功 | Windows実デコード、順逆シーク、実PTS索引、異常入力からの復帰 |
| audio integration | 98項目成功 | PCM精度、AAC、時刻補正、MP4編集表、失敗時後片付け |
| cache integration | 19項目成功 | SHA再利用・破損拒否・変更検出、A/V個別終端、先行音声 |
| sequence integration | 191項目成功 | 上記67単体項目を含む。109フレームのBGRA画素とPNG復号画素がバイト一致 |

これらの件数は同じ単体項目を含むため、単純合計を独立したテスト件数として扱いません。

実動画素材はCFR、VFR、Bフレーム、194×130の行余白、開始PTSが0でない入力、日本語パス、映像より長い音声を含みます。
独立したFFprobe / FFmpegの参照とも比較しています。異なるデコーダ同士の色比較はチャンネルごとに許容幅を設け、同一OSデコード結果をPNGへ保存する検査は全画素一致です。

音声ではPCM8/16/24/32、Float32、mono/stereo/5.1のキャッシュ生成、奇数長PCM24、AAC primingと末尾padding、先行empty edit、Bフレームと音声の原点を確認しました。
PCM24の元サンプルはWAVへ完全に保持されます。Float32とstereo PCM32は標準Storyの対応形式に合わせてPCM24となり、差が1量子化段階以内であることと精度変換の通知を確認しました。
複雑なMP4 edit、壊れた時刻表、未知の映像preroll、出力上限、取消、既存ファイル上書きは停止側を検証しています。

根拠：`build/extension-final-build-tests.log`、`build/Testing/Temporary/LastTest.log`、`build/fixtures/integration-results.json`、`build/audio-fixtures/integration-results.json`。

## MotionBuilder実ホスト

最終DLLで `./scripts/launch.ps1 -SmokeTest` を実行し、直接再生の38項目が成功しました。
背景・前景・テクスチャ、通常カメラとCamera Switcher、日本語パス、標準マテリアル接続、実GL画素、速度・ループ・逆再生・トリム、FBX再Open・接続復元、停止後の保持、欠落ファイルからの復帰、共有参照とruntime解放を含みます。
根拠：`artifacts/host-final-launch.log`、`artifacts/host-smoke/host-results.json`。

Storyは標準のFBVideoClipImage、FBStoryClip、FBAudioClipを使います。
プラグイン経由の実ホスト確認で画像シーケンス有効化、ソースFPS、A/Vそれぞれの長さ、移動・速度・トリム、Mute/Unmuteに応じた実GL画素の変化、PCM16/24とFloat32変換後の標準音声ロード、精度変換metadataを確認しました。
16bit stereoの標準Storyから実出力へ届く440HzをWASAPI loopbackで検出しました（振幅0.0496329）。

途中のStory検証ではPCM24素材の実際の431Hzに対してテストが660Hzを期待していたため停止しました。
PCM24の元音声と生成WAVのサンプルバイト一致および431Hzを独立解析し、期待周波数を修正しました。
修正後、利用者が明示的に指定した新しい検証用ウィンドウを1つだけ起動し、導入先DLLを使うStory通し検証58項目すべてが成功しました（24.87秒）。
標準Storyの実画素、移動・速度変更・トリム・Mute、440Hz / 431Hzの実音声出力、複数WAVのFBX再読込、A/V時刻と失敗時rollbackを確認しています。
検証時と当時の配布DLLのSHA256は一致し、成功後の動画・音声シーンを保存して検証ウィンドウを開いたまま残しました。
根拠：`artifacts/installed-story-acceptance/story-results.json`、`launch.log`、`install.json`、`build/audio-agent/precision-host-wave-check.json`。

同じ検証ウィンドウのPython Editorから `scripts/register_plugin_path.py` を実行し、FBConfigFileで導入済みDLLの検索パスを保存しました。
変更前の設定をバイト単位でバックアップし、SDKから設定を開き直して登録パスの一致を確認しました。
設定ファイルの変更は `[AdditionnalPluginPath]` のCountと追加パスだけで、それ以外の全文はバックアップと一致しています。
登録先DLLは検証済みDLLと同じSHA256でした。設定保存時にプロセスを追加・再起動せず、ダイアログを閉じてStory表示へ戻しました。
設定保存後の再起動による自動ロードは未確認です。
根拠：`artifacts/installed-story-acceptance/settings-results.json`、`settings-verification.json`。

標準SDK単独でも画像シーケンスの画素、移動・速度・トリム・Mute、FBX保存・再読込の10項目が成功しています。
根拠：`artifacts/story-sequence-final-probe-guard/results.json`。

## 既存ウィンドウの再利用

通常は既存のMotionBuilderを使います。後の利用者の明示的な依頼に応じたStory通し検証だけは、新しいウィンドウを1つ追加しました。
`launch.ps1` は既存プロセスがあればPIDを表示して終了し、初期化を伴うSmokeTest指定は既存プロセスがある限り拒否します。明示的な `-NewWindow` 指定時だけ、この起動制限を解除します。
`-StorySmokeTest -KeepOpen` は成功・失敗後も同じ検証ホストを残します。起動スクリプトの分岐・親環境のKeepOpen値を含む15項目のモック検証も成功しています。
`./scripts/launch.ps1` による既存PID検出とプロセス数不変を確認しました。

既存シーンに検証専用Storyフォルダを追加する `tests/mobu_reuse_checks.py` と外部音声プローブ用の `tests/run_reuse_checks.ps1` を用意しました。
この補助検証はFileNew / FileOpen / FileExitやコンポーネント削除を行いません。
既存のPython Editorから実行後、`pwsh -NoProfile -File tests/run_reuse_checks.ps1` が28項目成功しました。
検証済みのOS生成キャッシュを標準Storyへ接続し、stereo 16bitの440Hz（振幅0.0646522）とPCM24の431Hz（振幅0.0403973）を実出力で検出しました。
3種類のWAVを標準FBX保存・追加読込し、別々のパスと元のSHA256一致、標準音声の再生準備完了を確認しました。
元シーンのオブジェクト・Take・時刻・ループ・Story Mute・選択・ファイル名を復元し、追加した検証トラックをミュートしています。
前後のMotionBuilder PIDは同じ1つで、プロセスを追加していません。
根拠：`artifacts/reuse-host/driver.log`、`driver-results.json`、`reuse-results.json`（status=passed、scene_preserved=true）。

## 検出して修正した事例

- VFRのPTSを変えるOS video processorのフレームレート変換を無効化。
- 公称FPSの丸めによるフレーム境界ずれを実sample PTSの索引で解決。
- MP4のBフレームとedit listによる時刻原点を解析し、先行音声・映像開始offset・A/V個別終端を保持。
- FBVideoClipImageのコンストラクターだけでは画像がロードされず、明示的なFilename設定が必要な点に対応。
- Float32等を標準Story対応PCMへ変換し、精度低下を表示・metadataに記録。
- 複数WAVの標準FBXコピーで同名になる問題を入力SHAと時刻方式を含む名前で回避。
- 深いパスで失敗する作成途中フォルダ名を短いGUID形式へ変更。
- import失敗時は所有するStory要素だけをrollbackし、既存カメラ接続を復元。
- 日本語MSVCのshowIncludes出力に合わせた依存追跡prefixをconfigure時に検出し、クリーン再ビルド。

## 未確認・対象外

他PC、他のMotionBuilder版、macOS / Linux、任意のコーデック、HEVC / AV1 / ProRes、HDR、アルファ、長時間・高解像度の性能、GPU障害復旧、動画書出しは受入検証していません。
GPU利用要求は有効ですが、すべての入力でGPUデコードエンジンが実稼働したことを計測した結果ではありません。
5.1のWAV生成は検証しましたが、標準Story側の多チャンネル再生は保証対象に含めません。
専用UIのファイル選択ダイアログによる手操作は未確認です。UIと同じC API経路で読込を検証しました。

VFRは標準Storyの固定FPSにサンプリングするため、フレーム重複・間引きがあり得ます。
PNGはデコード後BGRA8画素の可逆保存です。Float32等の24bit化とVFR時刻まで可逆という意味ではありません。
配布ZIPにはDLL、導入・解除PS1、導入・検証・依存関係の文書、起動補助、SHA256 manifestを含め、SDK・検証素材・個人環境のログは含めません。

## 配布物検査

0.2.0は `./scripts/package.ps1 -SkipBuild` でZIPを生成しました。
5ファイルのみの構成、manifest内4ファイルのSHA256、実ホスト検証DLLとのバイト一致、個人パス非混入を確認しました。
対象ソース・文書のUTF-8 / LF / BOMなし、PythonとPowerShellの構文も検証しました。
根拠：`build/package-0.2.0.log`、`build/package-0.2.0-verification.json`。

## 0.2.1の導入・解除PS1

ZIPのルートに `Install.ps1` / `Uninstall.ps1`、共通処理として `scripts/PluginPath.ps1` を追加しました。
展開先のpluginsをユーザー設定へ登録・解除し、設定のバックアップを残します。
Windows標準PowerShell 5.1とPowerShell 7.6.5で `tests/plugin_path_tests.ps1` の47項目ずつが成功しました。
既存パスとコメント、UTF-8 / BOM / LF / CRLF、重複実行、SDK形式のパスエスケープ、WhatIf、起動中拒否、途中のホスト起動・外部設定変更、壊れた設定の拒否をfixtureで確認しました。

既存SDKのFBConfigFileを使う検証専用プログラムで設定ファイルを作り、配布ZIPから展開したInstall / UninstallをWindows PowerShell 5.1で実行しました。
日本語を含む展開先の登録を実SDKのGetで読み戻し、元のパスの維持と、解除後に設定バイト全体が原本と一致することを確認しました。
この検証は独立したfixtureのみを対象とし、その実行中だけホスト存在チェックをモックしています。MotionBuilder本体の起動・再起動は行っていません。
SDKと検証専用プログラムは配布物に含めません。

このPCの既存設定には `-WhatIf` だけを実行し、標準の設定ファイル検出、登録済み判定、解除予定の算出、設定SHA256とファイル一覧の不変を確認しました。
`./scripts/build.ps1 -SkipTests` は構成とビルドが成功し、DLLは再生成されず、0.2.0の実ホスト検証済みDLLと同じSHA256でした。
今回の変更に関係するCTestは `ctest --test-dir build -R plugin_path --output-on-failure` で実行し、2スイートとも成功しました。
実際のユーザー設定への登録・解除や、再起動後の自動ロードは今回追加検証していません。

根拠：`build/plugin-path-tests-ps51.json`、`build/plugin-path-tests-pwsh.json`、`build/plugin-path-ctest.log`、`build/sdk-registration-results.json`、`build/plugin-path-real-preview.json`、`build/installer-build.log`。

導入PS1を追加した時点の0.2.1 ZIPは8ファイルで構成し、manifestの7ファイルのSHA256、ソースと同梱PS1の一致、DLLの一致、個人パスとSDK・検証プログラムの非混入を検査しました。
根拠：`build/package-0.2.1.log`、`build/package-0.2.1-verification.json`。

## GitHub公開用のCIと手元DLLの配布

公開ソースだけを別フォルダへコピーし、固定したFFmpeg / FFprobe 9.0.2を使って `scripts/ci-build.ps1 -Mode Core` を実行しました。
Windows 11の既存MSVC / Windows SDKでReleaseビルドが成功し、CTestの13スイートすべてが成功しました（53.69秒）。
構成は `MOBU_BUILD_PLUGIN=OFF`、`MOBU_REQUIRE_INTEGRATION_TESTS=ON` で、MotionBuilder SDKへの参照は不要です。

今回の結果は、導入・解除47項目ずつ、配布物22項目ずつをWindows PowerShell 5.1とPowerShell 7で含みます。
公開ソース検査の回帰7項目、リリース入力の回帰15項目も成功しました。
リリース処理の最終修正後に関係する3スイートを再実行し、配布物22項目ずつと、実ワークフローのDraft状態・ID・既存添付物の検査を含むCI回帰24項目が成功しました。
動画実デコード73項目、音声98項目、キャッシュ19項目、PNGの画素比較を含むシーケンス191項目も成功しています。
新しいパッケージ処理はSDKやCMakeが使えないfixtureでも手元DLLから作成できることを確認しました。

深い作業フォルダでの予備検証では、WAVの一時出力がWindowsのパス長制限で失敗しました。
テストの出力先を環境の一時フォルダ配下の短い場所にし、同じ入力でキャッシュ19項目が成功しています。
製品側のパス長制約を解消したという結果ではありません。実利用でも短いキャッシュ保存先を選んでください。

配布DLLには、上記の実ホスト検証と同じSHA256 `fb3641ce9c2e2a31533f00150c125bd5ae359f530aca60cdd8f47f377de27502` のファイルを選びました。
`prepare-release-input.ps1` でDLLと検査用JSONを生成し、`ci-build.ps1 -Mode PackageInput -Tag v0.2.1` で照合とZIP作成が成功しました。
最終ZIPはmanifestを含む9ファイルで、全ハッシュ、DLLのx64形式、Windows PowerShell 5.1での構文、同梱ソースとの一致を確認しています。
公開ソース58ファイルのUTF-8 / LF / BOMなしと個人パス等の検査、ワークフローのactionlint検査も成功しました。

JSONはDLL・ソース集合の整合性を確認するための入力です。DLLのビルド証明にはなりません。
GitHub-hosted環境でのワークフロー実行・Draftへの添付・公開は、公開先が未指定のため未実施です。
ローカルCore検証は、そのGitHub環境での成功を保証するものではありません。
今回、MotionBuilderの追加起動や実ユーザー設定の変更は行っていません。

根拠：`build/public-cleanroom-core-final.log`、`build/public-cleanroom-release-focused-final.log`、`build/clean-source-final-inspection.json`、`build/release-input-tests.json`、`build/release-package-manifest-lf-pwsh.json`、`build/release-package-manifest-lf-winps51.json`、`build/public-package-input-final.log`、`build/ci-package-final/input-verification.json`、`build/ci-package-final/package-verification.json`、`build/final-distribution-audit.json`、`build/actionlint-results-final.log`。

## 日本語配置先とPowerShellの相対パスの修正

日本語・空白を含む別フォルダで、修正前のCMake構成がC1083で失敗することを再現しました。
構成時のコンパイラ呼出しにUTF-8を指定し、CMakeがその出力を読む際の文字コードもUTF-8に明示しました。
同じ配置先で構成とビルドが成功し、Ninjaの依存情報にプロジェクトのヘッダーが登録されていることを確認しました。
ヘッダーの内容を変えず更新日時だけを進めると、依存する2つのオブジェクトが再構築され、次のビルドは `no work to do` になりました。

PowerShellの現在位置とプロセスのCurrentDirectoryが異なる状態でも、相対パス引数を現在のPowerShellのFileSystemから解決するよう、開発・配布用の9スクリプトを修正しました。
同梱ファイルの追加やプロセス全体のCurrentDirectory変更は行いません。
日本語・空白・角括弧、PSDrive、FileSystem修飾パス、未作成の出力先、非FileSystemパスの拒否を回帰検証しました。
入力準備・検証23項目、配布物31項目ずつ（PowerShell 5.1 / 7）、CI処理29項目が成功しています。
既存の導入・解除47項目ずつ（PowerShell 5.1 / 7）も成功しました。

最新ソースを日本語・空白の配置先へコピーし、元フォルダをPATHから除外し、MOBU_ROOTを空にした状態で確認しました。
そのフォルダへ移動して `scripts/ci-build.ps1 -Mode Core -BuildDirectory ./build/core` を実行し、ReleaseビルドとCTestの13スイートがすべて成功しました（60.26秒）。
実DLLを使う入力準備、PackageInput、ZIP検査も、移動後の相対パス引数で成功しました。
レポートの保存先が移動後のフォルダであることと、プロセスのCurrentDirectoryが不変であることも確認しました。
通常のGitHub CIも、日本語・空白を含むフォルダへcheckoutして検証する設定にしました。GitHub上での実行は未確認です。

C++ソースは変更していません。更新した配布ZIPには実ホスト検証済みと同じSHA256のDLLを使い、最新のソース指紋を持つ入力JSONとZIPのmanifest・SHA256を再生成しました。
MotionBuilderの追加起動や実ユーザー設定の変更は行っていません。別PCでの受入試験は今回の範囲に含みません。

根拠：`build/path-fixes/japanese-configure-red.log`、`japanese-build-green.log`、`japanese-ninja-deps.log`、`japanese-header-incremental.log`、`japanese-no-work.log`、`japanese-core-all-green.log`、`relative-input-real-green.log`、`relative-package-real-green.log`、`relative-verify-real-green.log`、`build/relative-path-input-green.json`、`build/relative-path-package-green.json`、`build/relative-path-package-green-ps51.json`、`build/relative-path-ci-green.json`。

## MP4の映像・音声の独立した終端（2026-10-17）

edit listのないMP4で、映像2秒・12フレームに対して音声が約3.0293333秒ある場合、Story用の映像が19フレームまで延長される不具合を再現しました。
MP4のsttsとcttsをrun単位で走査し、最小の表示時刻と各サンプルの表示終端の最大値を求めるよう修正しました。
edit listの明示終端は維持し、edit listがない場合に映像・音声それぞれの終端を設定します。
修正後の同じ入力は映像2秒・12フレーム、音声約3.0293333秒となりました。
キャッシュ世代を5に更新し、旧世代の誤ったキャッシュを再利用しません。既存キャッシュは削除せず、既存Storyクリップへは元動画の再読込が必要です。
直接動画再生でコンテナ終端まで最終フレームを保持する既存の仕様は変更していません。

映像2秒／音声約3.03秒、映像3秒／音声約2.03秒、Bフレーム、VFRについて、edit listあり・なしの計8入力を追加しました。
独立したFFprobeのトラック時刻とedit listの解析を終端の参照値にし、音声はFFmpegの時刻を考慮したPCM出力とサンプル数・平均差・無音位置を照合しました。
Bフレームのno-edit入力に存在する16,000サンプルの時刻上の空白も保持しています。
非単調なsigned CTTS、可変サンプル期間、40億サンプルのrun、ゼロ期間、不正・オーバーフローも検査しました。
音声200項目とキャッシュ60項目が成功しています。

Windows 11、既存MSVCとMotionBuilder 2026 SDKで `scripts/build.ps1 -BuildDirectory ./build/track-ends-sdk -RequireIntegrationTests` を実行し、Release DLLの再ビルドとCTest全13スイートが成功しました（63.44秒）。
生成した動画fixtureは既存RTX 4070のh264_nvencを使用しています。
公開ソースのみを日本語・空白を含む別フォルダへコピーし、SDKを使わず `scripts/ci-build.ps1 -Mode Core -BuildDirectory ./build/core` を実行した結果も13スイート成功しました（62.78秒）。
この再検証は既存ホストが参照しているfixtureを上書きしないよう、新しいビルド先で実施しています。

今回再生成したDLLのSHA256は `795bb13f23c731a25af261e3bc97402f323a4e97a3a7ba034afcbf1c9bbe94ae` です。
READMEから製品の固定版数を除き、GitタグとReleaseで配布版を示す説明に変更しました。

この新DLLだけを指定した専用のMotionBuilder 2026検証ウィンドウを1つ起動し、既存ウィンドウは変更していません。
設定保存先も検証用フォルダへ分離し、マウス・キー入力なしでSDKから試験しました。
まず `tests/mobu_story_smoke.py` の描画・実音声出力・標準Story操作・FBX再読込の58項目が成功しました。
続けて同じプロセスで `tests/mobu_story_track_ends.py` を実行し、上記8入力の読込、映像と音声双方の移動・トリム・速度・Muteと復元、PNG枚数・WAVサンプル数とキャッシュハッシュ、FBX保存・新規シーン・再読込の596項目が成功しました。
実際にロードされたDLLと選択したDLLのSHA256一致も検査しています。
映像2秒／音声3.0293333秒はPNG12枚、映像3秒／音声2.0266667秒はPNG18枚となり、再読込後も各終端を保持しました。
VFRは映像1.88秒／音声3.0293333秒、Bフレームno-edit入力は映像開始0.3333333秒・長さ2秒／音声3.3626667秒を保持しています。
再読込後の試験FBXを別名保存し、追加した終端検証トラックをMuteにして同じウィンドウを残しています。
検証スクリプトは専用ホストを示す環境変数と期待DLLハッシュを必須にしています。シーンを切り替えるため、共有作業中のホストでは実行しないでください。

根拠：`build/remaining-review-noedit/raw-aac-comparison.json`、`fixed-raw-aac-comparison.json`、`build/track-ends-audio-red.log`、`track-ends-cache-red.log`、`track-ends-fresh-sdk.log`、`track-ends-sdk/audio-fixtures/track-ends-reference.json`、`track-ends-sdk/Testing/Temporary/LastTest.log`、`track-ends-japanese-core.log`、`artifacts/ends-host-001/story-results.json`、`ends/track-ends-results.json`、`ends/track-ends-reopened.fbx`。

## 導入済みDLLの更新と通常起動での確認（2026-10-17）

開いていた専用検証シーンを別名保存し、旧DLLと登録設定をバックアップしてから、登録済みの導入先を上記SHA256の新版DLLへ更新しました。
検証ウィンドウだけを一度再起動し、テスト用のプラグイン検索パスと設定保存先の上書きなしで確認しました。
明示的にDLLを読み込む前に、自動ロード済みのモジュールが通常の登録先にあることと、配布対象DLLとのハッシュ一致を検査しました。

導入とシーン復元の65項目、Storyの描画・実音声出力・標準編集・FBX再読込の58項目、映像と音声の長さが異なる8種類のMP4の596項目が、再起動後の同じプロセスですべて成功しました。
最後に更新前の24本のStoryトラックと全クリップの開始・終了・トリム・速度・Mute、Takeを復元し、保存時と一致することを確認して別名保存しました。
独立監査も29項目成功しました。既存の別作業ホストを停止・再起動する操作は行っていません。

DLL更新時には登録設定のバイト一致を確認しました。通常起動後には設定ファイルの空行数が変わりましたが、417行の非空行の順序・内容は一致しています。
この導入確認ではソースとDLLを再生成していません。GitHub-hosted CIの実行結果は各ReleaseとActionsで確認してください。

根拠：`artifacts/installed-update-001/scene-backup.json`、`installed-update.json`、`restart.json`、`host/installed-host-results.json`、`host/story-results.json`、`host/ends/track-ends-results.json`、`independent-audit.json`。

## GitHub CIの初回実行で見つかった問題（2026-10-17）

GitHubのWindows Server 2022ランナーで、Visual StudioのDevShell初期化時に、先に追加したFFmpegの検索パスが消える失敗を確認しました。
実DevShellを使ってPowerShell 5.1と7で再現し、初期化前のPATHをVSのPATHへ追加して両方のツールを保持するよう修正しました。
実build.ps1を呼ぶ回帰テストは修正前に失敗し、修正後はCI契約テスト30項目が成功しました。
英語環境のPython標準出力で日本語の音声テスト名を表示できない問題も再現し、UTF-8を明示した後は同じcp1252環境指定で音声200項目が成功しました。

開始時刻のずれを加えたシーク位置がコンテナ終端を超えると、OSのMedia Sourceが要求を拒否する問題をソフトウェアデコードでも再現しました。
OSへ渡すシーク位置を有効な終端に収め、そこから目的の表示フレームまでデコードするよう修正しました。
末尾表示後に末尾直前へ戻るケースを7入力へ追加し、同じソフトウェア検証は修正前の78成功・2失敗から80項目成功になりました。

修正後のソースから既存MotionBuilder 2026 SDKで新しいRelease DLLを生成し、CIと同じFFmpeg/FFprobe 9.0.2でCTest全13スイートが成功しました（67.65秒）。
デコーダは80項目、音声は200項目が成功しています。
今回の配布対象DLLのSHA256は `0a73b0a0545bf9771c1ff51dd90c51f818308f8bb0ee370d4347312b39bd1ce8` です。
DLL内の個人パス・PC名・認証情報のパターン検査も成功しました。

このDLLへ登録先を更新する前に、既存の専用検証ウィンドウの24本のStoryトラックを別名保存しました。
その検証ウィンドウだけを1回再起動し、通常設定からの自動ロードと配布DLLのハッシュ一致を確認しました。
導入とシーン復元の68項目、Storyの58項目、長さの異なるMP4の8入力596項目が成功し、元の24トラックと編集状態を復元して別名保存しました。
マウス・キー入力は使用せず、別作業のホストには変更を加えていません。

根拠：`build/github-ci-path-red.json`、`github-ci-path-green.json`、`ci-audio-encoding-red.log`、`ci-audio-encoding-green.log`、`server-seek-probe/regression-before.log`、`server-seek-probe/regression-after.log`、`server-seek-sdk-build.log`、`server-seek-sdk/Testing/Temporary/LastTest.log`、`artifacts/release-validation-001/scene-backup.json`、`host/installed-host-results.json`、`host/story-results.json`、`host/ends/track-ends-results.json`。

## OSのMP4ソースが移動するAAC時刻原点（2026-10-17）

GitHubのWindows Server 2022では、同じAACファイルの圧縮入力とPCM出力がともに213,333単位（100ns）の時刻から始まり、Windows 11ではともに0から始まる差を観測しました。
両環境で圧縮パケット48個・PCM 49,152フレームと先頭波形は一致しました。
従来処理はAACのprimingを正しく除去できず、1,024サンプルの遅延となっていました。独立参照との平均差は約5,235.88、最適な比較位置の差は1,024サンプルで、位置を合わせた平均差は約0.168でした。

静的なMP4時刻表を持つAACについて、OSが返す最初の圧縮パケットの時刻とファイル内の時刻表を照合し、その差をPCMの時刻へ補正します。
OSの版名や1,024という固定値では分岐せず、edit listとサンプル間の空白は従来どおり適用します。
キャッシュ世代を6に更新し、補正前のキャッシュを再利用しません。
標準出力の診断に平均差・相関・位相差などを加え、既存の平均3量子化レベル以内という品質基準は維持しました。
時刻原点の回帰12項目を追加し、Windows 11で音声212項目とCTest全13スイートが成功しました（67.58秒）。
修正を反映した[GitHub ActionsのWindows Server 2022実行](https://github.com/MizoTake/MoBuVideoPlayer/actions/runs/39014008025)でも、CTest全13スイート、デコーダ80項目、音声212項目、公開ソース59ファイルの検査が成功しました。

AACの元のprimingに加えて可聴部分の先頭0.5秒を切るedit listのケースも追加しました。
FFprobeのSkipSamples、ファイルの物理先頭パケットとOSが返す先頭パケットのSHA256を独立に照合し、先頭パケットが破棄されていないことを確認しました。
出力は24,000フレーム・0.5秒で、参照との位相差0、平均差約0.226でした。追加後の音声218項目も成功しました。
修正後のSDKビルドはCTest全13スイートが成功しました（65.44秒）。
最終配布候補DLLのSHA256は `7b03feb2801fbc58867211c2cac81b96bc2da1c1d566de68974756673c5ab24c` です。

根拠：`build/github-ci-failure-3.log`、`github-ci-failure-4.log`、`aac-reader-win11.json`、`aac-clock-local-regression.log`、`aac-clock-core-ctest.log`、`github-aac-clock-ci.log`、`github-green-core/public-ci-summary.json`、`aac-trim-regression.log`、`aac-clock-sdk-build.log`。
