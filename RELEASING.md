# GitHubへの公開とリリース

手元で動作を確認したDLLを入力として、導入・解除PS1入りZIPとSHA256をGitHubのDraft Releaseへ添付します。
CIはGitHub-hostedの `windows-2022` で動き、MotionBuilder SDKやセルフホストランナーを必要としません。
プラグインDLLの再ビルドは行いません。
公開先は [MizoTake/MoBuVideoPlayer](https://github.com/MizoTake/MoBuVideoPlayer) です。配布版はGitタグとReleaseで管理します。

## 通常のCI

`.github/workflows/ci.yml` はPR・ブランチ更新・手動実行に対応します。
日本語と空白を含むフォルダへcheckoutし、その配置でビルドとテストを行います。
SDKを使わずにネイティブメディア処理をビルドし、動画・音声、導入・解除PS1、配布物、公開ソース、リリース入力を検証します。
Python 3.10以降・FFmpeg・FFprobeを必須とし、比較テストを省略しません。

GitHub-hostedのCoreジョブでだけ、不足するFFmpeg / FFprobeを固定バージョンとSHA256で検証して取得します。
Windows ServerのMedia Foundation機能も確認し、必要ならその一時ランナーで有効化します。再起動が必要なら失敗します。
ローカルPCのOS設定変更やSDK取得は行いません。
FFmpegはテスト用で、製品ZIPにも再生処理にも含みません。

## 手元DLLと検査用JSONの準備

対象はWindows x64 / MotionBuilder 2026用のDLLです。
配布するDLLを選び、対応するソースと実ホストでの検証記録を確認してください。
ローカルで再ビルドする場合のSDK設定はREADMEを参照してください。既存DLLからのパッケージ作成にはSDKは不要です。

~~~powershell
./scripts/check-public-source.ps1
./scripts/ci-build.ps1 -Mode Core
./scripts/prepare-release-input.ps1 -PluginPath ./build/package/plugins/MoBuVideoPlayer.dll
~~~

`dist/release-input` に次の2ファイルを作成します。

- `MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll`
- `MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll.json`

JSONには版数、対象ホスト、DLLのSHA256、CMakeLists.txtとsrc/include内のC++ソース集合のSHA256を記録します。
ファイル集合・内容がタグと異なる場合や、DLLの改変・非x64形式・版数違いを検出した場合はパッケージ作成を拒否します。
この照合は、選んだDLLがそのソースからコンパイルされたことの証明にはなりません。DLLの選択と対応確認は管理者が手元の検証記録に基づいて行います。
ソースの変更後は、対応を確認して入力を作り直してください。
入力DLL・JSON・build・distはGitへコミットしません。

CIと同じパッケージ処理をローカルで実行できます。

~~~powershell
./scripts/ci-build.ps1 -Mode PackageInput -Tag v0.2.1 `
    -PluginPath ./dist/release-input/MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll `
    -MetadataPath ./dist/release-input/MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll.json
~~~

## GitHubでのリリース手順

1. CMakeLists.txtの版数と `docs/releases/<版数>.md`、検証記録を揃えます。配布版の表示はGitタグとReleaseにまとめ、READMEに製品の固定版数は書きません。
2. 上記の検査・入力準備を実行し、公開対象のソースをコミットします。
3. originの公開先を確認し、ソースと版数タグをpushします。
4. 同じタグのDraft Releaseを作り、DLLとJSONの2点を添付します。
5. `release.yml` をタグ指定で手動実行します。

以下の版数は対象リリースに合わせてください。

~~~powershell
$releaseRepo = 'MizoTake/MoBuVideoPlayer'
git tag v0.2.1
git push origin main
git push origin v0.2.1
gh release create v0.2.1 `
    ./dist/release-input/MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll `
    ./dist/release-input/MoBuVideoPlayer-0.2.1-Mobu2026-win64.dll.json `
    --repo $releaseRepo --draft --verify-tag --title 'MoBuVideoPlayer 0.2.1' `
    --notes-file docs/releases/0.2.1.md
gh workflow run release.yml --repo $releaseRepo --ref main -f tag=v0.2.1
~~~

`.github/workflows/release.yml` は指定タグと未公開Draftの存在を確認し、そのタグのソースをcheckoutします。
Coreのビルド・テスト後、同じDraftから固定名のDLL・JSONだけを取得し、照合とパッケージ作成を行います。
添付直前にも同じRelease IDとタグ、Draft状態を確認します。公開状態の変更や既存の添付物の自動置換は行いません。
Actions標準の `GITHUB_TOKEN` を使うため、追加のPersonal Access TokenやSDK用ランナー設定は不要です。
入力アップロード前には実行せず、タグとDraftの入力を揃えてから手動実行してください。
ZIPが既に添付されているDraftを作り直す場合は、管理者がDraft状態を確認し、古いZIPとSHA256を削除してから再実行してください。
処理中に他の管理者がPublishすると状態確認との競合が起こり得るため、CI完了までDraftを保持してください。

## 配布物と公開前の確認

CIが同じDraftへ追加する成果物は次の2点です。入力DLL・JSONも添付物として残します。

- `MoBuVideoPlayer-0.2.1-Mobu2026-win64.zip`：DLL、Install.ps1、Uninstall.ps1、共通PS1、起動補助、利用・検証・依存関係の文書、manifestの計9ファイル。
- `MoBuVideoPlayer-0.2.1-Mobu2026-win64.zip.sha256`：ZIPのSHA256。

ZIPはファイル名の許可リスト、manifestの全ハッシュ、DLLのx64 PE形式、Windows PowerShell 5.1の構文、同梱PS1とソースの一致を検査します。
検証ログの公開成果物は相対名と件数を含むサマリーで、生の絶対パスを含むテストログは添付しません。
CIの検査はMotionBuilderでの画面操作・実音声出力の受入試験とは別です。
DraftのDLL、ZIP、検証結果、リリース説明を確認してからPublishしてください。

本プロジェクト自身のライセンスは未選択です。現時点ではLICENSEやMIT等の利用許諾を付与していません。
依存関係と配布範囲は [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) を参照してください。
