# build_apk.ps1 —— 手工构建 Android APK（不需要 Gradle）
#
# 为什么不用 Gradle：本机只装了 JDK 17 + Android SDK 的 build-tools/platform-tools，
# 没有 Gradle，也没有网络去下 Gradle 发行包。而一个纯 Java、零依赖的 App
# 完全可以用官方工具链一步步拼出来：
#
#   aapt2 compile  资源 png/xml -> 每个资源一个 .flat
#   aapt2 link     .flat + AndroidManifest.xml -> 未签名的 APK（含 resources.arsc 与 R.java）
#   javac          编译 src/*.java + gen/R.java -> .class
#   d8             .class -> classes.dex
#   打包            把 classes.dex 塞进 APK（.NET ZipArchive 追加）
#   zipalign       4 字节对齐（Android 装包要求）
#   apksigner      v1+v2 签名（用 android/novelreader.keystore）
#
# 用法（PowerShell 5.1）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File android\build_apk.ps1
#
# 产物：build\NovelReader-1.7.0.apk

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot          # 仓库根目录
$proj    = Join-Path $root 'android'
$out     = Join-Path $root 'build'
$work    = Join-Path $out 'apkwork'

$sdk     = 'E:\Android\sdk'
$jdk     = 'E:\Android\jdk\jdk-17.0.13+11'
$aapt2   = Join-Path $sdk 'build-tools\34.0.0\aapt2.exe'
$d8      = Join-Path $sdk 'build-tools\34.0.0\d8.bat'
$zipalign= Join-Path $sdk 'build-tools\34.0.0\zipalign.exe'
$apksign = Join-Path $sdk 'build-tools\34.0.0\apksigner.bat'
$androidJar = Join-Path $sdk 'platforms\android-34\android.jar'
$javac   = Join-Path $jdk 'bin\javac.exe'
$keytool = Join-Path $jdk 'bin\keytool.exe'

$apkName = 'NovelReader-1.7.0.apk'
$finalApk = Join-Path $out $apkName

foreach ($t in @($aapt2, $d8, $zipalign, $apksign, $androidJar, $javac)) {
    if (-not (Test-Path $t)) { throw "缺少构建工具：$t" }
}

# d8.bat / apksigner.bat 内部会去调 java.exe，得让它们能找到 JDK。
# 这里直接改进程环境变量；比写 cmd 的 `set "JAVA_HOME=..."` 少一个"空格被吃进值里"的坑。
$env:JAVA_HOME = $jdk
$env:PATH = "$jdk\bin;$env:PATH"

# 清空工作目录（旧产物会让 aapt2/d8 认错文件）
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$genDir = Join-Path $work 'gen'
$clsDir = Join-Path $work 'classes'
$dexDir = Join-Path $work 'dex'
New-Item -ItemType Directory -Force -Path $genDir, $clsDir, $dexDir | Out-Null

Write-Output '=== 1/6 aapt2 compile（资源） ==='
$flat = Join-Path $work 'res.zip'
& $aapt2 compile --dir (Join-Path $proj 'res') -o $flat
if ($LASTEXITCODE -ne 0) { throw "aapt2 compile 失败（$LASTEXITCODE）" }

Write-Output '=== 2/6 aapt2 link（生成 APK 骨架 + R.java） ==='
$unsigned = Join-Path $work 'unsigned.apk'
& $aapt2 link -o $unsigned -I $androidJar --manifest (Join-Path $proj 'AndroidManifest.xml') `
    --java $genDir --auto-add-overlay $flat
if ($LASTEXITCODE -ne 0) { throw "aapt2 link 失败（$LASTEXITCODE）" }

Write-Output '=== 3/6 javac（Java 源码） ==='
$srcs = @(Get-ChildItem (Join-Path $proj 'src') -Recurse -Filter *.java | ForEach-Object { $_.FullName })
$srcs += @(Get-ChildItem $genDir -Recurse -Filter *.java | ForEach-Object { $_.FullName })
Write-Output ("    源文件 " + $srcs.Count + " 个")
# -source 8 -target 8：Android 21+ 的运行时只保证 Java 8 的字节码特性
& $javac -encoding UTF-8 -source 8 -target 8 -bootclasspath $androidJar -d $clsDir $srcs
if ($LASTEXITCODE -ne 0) { throw "javac 失败（$LASTEXITCODE）" }

Write-Output '=== 4/6 d8（转成 dex） ==='
$classes = @(Get-ChildItem $clsDir -Recurse -Filter *.class | ForEach-Object { $_.FullName })
& $d8 --lib $androidJar --min-api 21 --output $dexDir $classes
if ($LASTEXITCODE -ne 0) { throw "d8 失败（$LASTEXITCODE）" }
$dex = Join-Path $dexDir 'classes.dex'
if (-not (Test-Path $dex)) { throw 'd8 没有产出 classes.dex' }

Write-Output '=== 5/6 把 classes.dex 塞进 APK ==='
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$withDex = Join-Path $work 'withdex.apk'
Copy-Item $unsigned $withDex -Force
$zip = [System.IO.Compression.ZipFile]::Open($withDex, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    # 先删掉可能存在的同名条目，再以"已压缩"方式写入
    $old = $zip.GetEntry('classes.dex')
    if ($old) { $old.Delete() }
    $entry = $zip.CreateEntry('classes.dex', [System.IO.Compression.CompressionLevel]::Optimal)
    $es = $entry.Open()
    $bytes = [System.IO.File]::ReadAllBytes($dex)
    $es.Write($bytes, 0, $bytes.Length)
    $es.Close()
} finally {
    $zip.Dispose()
}

Write-Output '=== 5.5/6 zipalign（4 字节对齐） ==='
$aligned = Join-Path $work 'aligned.apk'
& $zipalign -f 4 $withDex $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign 失败（$LASTEXITCODE）" }

Write-Output '=== 6/6 apksigner（签名） ==='
$ks = Join-Path $proj 'novelreader.keystore'
if (-not (Test-Path $ks)) {
    Write-Output '    第一次构建：生成自签名密钥库（密码 novelreader，别名 novelreader）'
    & $keytool -genkeypair -v -keystore $ks -alias novelreader `
        -keyalg RSA -keysize 2048 -validity 10000 `
        -storepass novelreader -keypass novelreader `
        -dname 'CN=NovelReader, OU=NovelReader, O=NovelReader, L=CN, ST=CN, C=CN'
    if ($LASTEXITCODE -ne 0) { throw "keytool 失败（$LASTEXITCODE）" }
}

if (Test-Path $finalApk) { Remove-Item $finalApk -Force }
& $apksign sign --ks $ks --ks-pass pass:novelreader --key-pass pass:novelreader `
    --ks-key-alias novelreader --out $finalApk $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner 失败（$LASTEXITCODE）" }

Write-Output '=== 校签 ==='
& $apksign verify --verbose $finalApk | Select-Object -First 6

$len = (Get-Item $finalApk).Length
Write-Output ("=== 输出: {0}  ({1} 字节, {2:N1} KB) ===" -f $finalApk, $len, ($len / 1KB))
