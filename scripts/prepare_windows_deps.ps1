$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

$CacheDir = if ($env:ENHANCER_WIN_DEPS_CACHE) {
    $env:ENHANCER_WIN_DEPS_CACHE
} else {
    Join-Path $HOME ".cache\enhancer-windows-deps"
}

$DepsDir = Join-Path $RepoRoot "windows\deps"
$NcnnExtractDir = Join-Path $DepsDir "ncnn-win-x64"
$StagedModelsDir = Join-Path $DepsDir "models"

$NcnnVer = "20261020"
$CfCommit = "cfe0fe2d247c05f7e7de00fab9ee55030315eac0"
$EncSha256 = "15a5b7b73a22ab0f54f9270c88367c53e568dc3f46e532fabb0028845b7876ff"
$GenSha256 = "34af3c500aa6912597867d1bd1bc2f4c98c7f9ad82fa39c045047b48989b9b3c"

New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null
New-Item -ItemType Directory -Force -Path $StagedModelsDir | Out-Null

function Test-FileSha256([string]$Path, [string]$ExpectedHex) {
    if (-not (Test-Path $Path)) { return $false }
    $actual = (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    return ($actual -eq $ExpectedHex.ToLowerInvariant())
}

# 1. Detect installed Visual Studio version (VS2022 v17 -> vs2022, VS2026 v18 -> vs2026)
$VsTag = "vs2022"
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $VsWhere) {
    $VsVer = (& $VsWhere -latest -property installationVersion) | Select-Object -First 1
    Write-Host "[prepare_windows_deps] Detected Visual Studio version: $VsVer"
    if ($VsVer -match "^18\.") {
        $VsTag = "vs2026"
    }
}

# Download & cache prebuilt ncnn-20261020-windows-$VsTag.zip (Vulkan SimpleVK + OpenMP + AVX2/AVX512)
$CachedNcnnZip = Join-Path $CacheDir "ncnn-$NcnnVer-windows-$VsTag.zip"
if (-not (Test-Path $CachedNcnnZip)) {
    $NcnnUrl = "https://github.com/Tencent/ncnn/releases/download/$NcnnVer/ncnn-$NcnnVer-windows-$VsTag.zip"
    Write-Host "[prepare_windows_deps] Downloading $NcnnUrl ..."
    curl.exe -fsSL --retry 3 --retry-delay 2 -o $CachedNcnnZip $NcnnUrl
}

$NcnnConfigCheck = Join-Path $NcnnExtractDir "x64\lib\cmake\ncnn\ncnnConfig.cmake"
$NcnnMarker = Join-Path $NcnnExtractDir ".vstag-$VsTag"
if ((-not (Test-Path $NcnnConfigCheck)) -or (-not (Test-Path $NcnnMarker))) {
    Write-Host "[prepare_windows_deps] Extracting ncnn Windows $VsTag SDK..."
    $TmpNcnn = Join-Path $DepsDir "_tmp_ncnn"
    if (Test-Path $TmpNcnn) { Remove-Item -Recurse -Force $TmpNcnn }
    Expand-Archive -Path $CachedNcnnZip -DestinationPath $TmpNcnn -Force
    if (Test-Path $NcnnExtractDir) { Remove-Item -Recurse -Force $NcnnExtractDir }
    Move-Item -Path (Join-Path $TmpNcnn "ncnn-$NcnnVer-windows-$VsTag") -Destination $NcnnExtractDir
    Set-Content -Path $NcnnMarker -Value $VsTag
    Remove-Item -Recurse -Force $TmpNcnn
}
Write-Host "[prepare_windows_deps] Verified ncnn ($VsTag) x64 CMake SDK at $NcnnConfigCheck"

# 2. Download & SHA-256 verify CodeFormer FP16 weights (encoder.bin + generator.bin)
$CachedEnc = Join-Path $CacheDir "codeformer_encoder.bin"
$CachedGen = Join-Path $CacheDir "codeformer_generator.bin"

if ((-not (Test-FileSha256 $CachedEnc $EncSha256)) -or (-not (Test-FileSha256 $CachedGen $GenSha256))) {
    $TarUrl = "https://codeload.github.com/FeiGeChuanShu/CodeFormer-ncnn/tar.gz/$CfCommit"
    Write-Host "[prepare_windows_deps] Fetching CodeFormer weights ($CfCommit)..."
    $TmpCf = Join-Path $DepsDir "_tmp_cf"
    if (Test-Path $TmpCf) { Remove-Item -Recurse -Force $TmpCf }
    New-Item -ItemType Directory -Force -Path $TmpCf | Out-Null

    $TarPath = Join-Path $TmpCf "cf.tar.gz"
    curl.exe -fsSL --retry 3 --retry-delay 2 -o $TarPath $TarUrl
    tar.exe -xzf $TarPath -C $TmpCf "CodeFormer-ncnn-$CfCommit/models/encoder.bin" "CodeFormer-ncnn-$CfCommit/models/generator.bin"

    Copy-Item -Force (Join-Path $TmpCf "CodeFormer-ncnn-$CfCommit\models\encoder.bin") $CachedEnc
    Copy-Item -Force (Join-Path $TmpCf "CodeFormer-ncnn-$CfCommit\models\generator.bin") $CachedGen
    Remove-Item -Recurse -Force $TmpCf

    if (-not (Test-FileSha256 $CachedEnc $EncSha256)) {
        throw "SHA-256 verification failed for codeformer_encoder.bin"
    }
    if (-not (Test-FileSha256 $CachedGen $GenSha256)) {
        throw "SHA-256 verification failed for codeformer_generator.bin"
    }
}

# 3. Stage all 20 neural model files (8 SR models + SCRFD + CodeFormer) into windows\deps\models
$RepoAssetsModels = Join-Path $RepoRoot "app\src\main\assets\models"
Get-ChildItem -Path $RepoAssetsModels -File | ForEach-Object {
    Copy-Item -Force $_.FullName (Join-Path $StagedModelsDir $_.Name)
}
Copy-Item -Force $CachedEnc (Join-Path $StagedModelsDir "codeformer_encoder.bin")
Copy-Item -Force $CachedGen (Join-Path $StagedModelsDir "codeformer_generator.bin")

$ModelCount = (Get-ChildItem -Path $StagedModelsDir -File).Count
Write-Host "[prepare_windows_deps] Staged $ModelCount neural model files in $StagedModelsDir"
