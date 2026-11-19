# Xenoblade Chronicles 3 (recomp) - installer and build. Started by "Install XC3.bat" (players: double-click it) and
# by build-windows.bat (developers, -BuildOnly / -ToolsOnly).
#
# Nothing has to be installed first: everything it needs goes into .tools\ in this folder (pinned versions, checked
# against their SHA-256): a private Python (python-build-standalone) with capstone / CMake / Ninja, and the llvm-mingw
# compiler (clang + lld, no Visual Studio). FFmpeg goes to third_party\ffmpeg. Then:
#   1. XC3 Setup (built here) installs YOUR game files into game\ - base game, update 2.2.1, DLC, from .xci / .nsp /
#      .nca or an Eden / yuzu-based emulator (found automatically, with its keys and saves)
#   2. recomp\recompile.py turns game\main.elf into C (gen\), compiled with the runtime into xc3.exe (10-40 min the
#      first time, minutes afterwards)
#   3. bin\ gets xc3.exe + "XC3 Launcher.exe", with desktop / Start menu shortcuts
# Run it again at any time: finished steps are skipped (resume after an interruption, rebuild after an update).
param(
    [switch]$BuildOnly,   # build-windows.bat: no shortcuts, no bin\, don't start the game
    [switch]$ToolsOnly,   # build-windows.bat tools: Setup + launcher only
    [switch]$NoLaunch,    # don't start the launcher at the end
    [switch]$Setup        # open XC3 Setup even when the game files are installed (add DLC, reinstall)
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # (Invoke-WebRequest is many times slower with its progress bar)
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $Root
$T = Join-Path $Root '.tools'
$DL = Join-Path $T 'downloads'
New-Item -ItemType Directory -Force $DL | Out-Null

# ---------------------------------------------------------------- pinned downloads
$PY_URL  = 'https://github.com/astral-sh/python-build-standalone/releases/download/20251031/cpython-3.12.12+20251031-x86_64-pc-windows-msvc-install_only.tar.gz'
$PY_SHA  = 'cff398b3f520c442a1b085dd347126c10c1b03f01ccc0decd8c897a687e893f1'
$PY_PKGS = @('capstone==5.0.9', 'ninja==1.13.0', 'cmake==3.31.6')
$LM_VER  = '20251104'
$LM_URL  = "https://github.com/mstorsjo/llvm-mingw/releases/download/$LM_VER/llvm-mingw-$LM_VER-ucrt-x86_64.zip"
$LM_SHA  = '171b298f46fa4b5653dc7b01cc5e709caca645991be8e1b0724466c4a282a1ac'
$FF_URL  = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-31-12-59/ffmpeg-n8.1.3-27-g8dca1586f4-win64-lgpl-shared-8.1.zip'
$FF_SHA  = '5cef7bc48e513f3cee7c881e8b2999d835541edb1846c3e89443a67f8f8556ab'
$GAME_BUILD_ID = '82D187FE9EF9BE92'   # the game executable the code is recompiled from (update 2.2.0 / 2.2.1)

function Say($m)  { Write-Host $m }
function Step($m) { Write-Host ''; Write-Host "== $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "   $m" -ForegroundColor Green }
function Fail($m) {
    Write-Host ''
    Write-Host "ERROR: $m" -ForegroundColor Red
    Write-Host 'Fix that and run the installer again: it continues where it stopped.' -ForegroundColor Yellow
    exit 1
}
function Run([string]$exe, [string[]]$a, [string]$what) {
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # (tools write warnings to stderr)
    & $exe @a
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($rc -ne 0) { Fail "$what failed (exit code $rc) - see the messages above." }
}
function Fwd([string]$p) { return ($p -replace '\\', '/') }

function Get-Pinned([string]$url, [string]$sha, [string]$name) {
    $out = Join-Path $DL $name
    if ((Test-Path $out) -and ((Get-FileHash $out -Algorithm SHA256).Hash -eq $sha)) { return $out }
    Say "   downloading $name ..."
    $part = "$out.part"
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    for ($i = 1; $i -le 3; $i++) {
        try {
            if (Test-Path $curl) {
                & $curl -L --fail --silent --show-error --retry 3 -o $part $url
                if ($LASTEXITCODE -ne 0) { throw "curl exit code $LASTEXITCODE" }
            } else {
                Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
            }
            break
        } catch {
            if ($i -eq 3) { Fail "Could not download $url ($($_.Exception.Message)). Check your internet connection." }
            Start-Sleep -Seconds 3
        }
    }
    $h = (Get-FileHash $part -Algorithm SHA256).Hash
    if ($h -ne $sha) { Remove-Item -Force $part; Fail "The download of $name is not the expected file (checksum $h)." }
    Move-Item -Force $part $out
    return $out
}
function Unpack([string]$archive, [string]$dest, [int]$strip = 0) {
    New-Item -ItemType Directory -Force $dest | Out-Null
    $a = @('-xf', $archive, '-C', $dest)
    if ($strip) { $a += "--strip-components=$strip" }
    Run (Join-Path $env:SystemRoot 'System32\tar.exe') $a "Unpacking $(Split-Path $archive -Leaf)"
}
function Free-GB([string]$path) {
    try { $d = (Get-Item $path).PSDrive; return [math]::Round(((Get-PSDrive $d.Name).Free) / 1GB, 1) } catch { return -1 }
}

Write-Host ''
Write-Host 'Xenoblade Chronicles 3 - recompiled PC version: install / build' -ForegroundColor White
Write-Host "Folder: $Root"
if ($Root -match '[^\x20-\x7e]') { Say '   (note: this folder path has non-English characters; if the build fails, move the folder to e.g. C:\Games\xc3-recomp)' }
if (!(Test-Path (Join-Path $env:SystemRoot 'System32\tar.exe'))) { Fail 'Windows 10 (version 1803) or newer is needed (tar.exe is missing).' }

# ---------------------------------------------------------------- update this folder (git clones)
if (!$BuildOnly -and (Test-Path (Join-Path $Root '.git')) -and (Get-Command git -ErrorAction SilentlyContinue)) {
    Step 'Updating (git pull)'
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & git -C $Root pull --ff-only
    if ($LASTEXITCODE -ne 0) { Say '   (could not update: local changes? continuing with this version)' }
    $ErrorActionPreference = $old
}

# ---------------------------------------------------------------- tools
Step 'Tools (downloaded once into .tools\)'
$Py = Join-Path $T 'python\python.exe'
if (!(Test-Path $Py)) {
    $a = Get-Pinned $PY_URL $PY_SHA 'python.tar.gz'
    Unpack $a $T
    if (!(Test-Path $Py)) { Fail 'Python did not unpack as expected.' }
}
$stamp = Join-Path $T 'python\xc3-packages.txt'
$want = ($PY_PKGS -join ' ')
if (!(Test-Path $stamp) -or ((Get-Content $stamp -Raw).Trim() -ne $want)) {
    Say '   Python packages: capstone, CMake, Ninja'
    Run $Py (@('-m', 'pip', 'install', '--disable-pip-version-check', '--no-warn-script-location', '-q') + $PY_PKGS) 'Installing the Python packages'
    Set-Content -Path $stamp -Value $want
}
$CMake = Join-Path $T 'python\Scripts\cmake.exe'
$Ninja = Join-Path $T 'python\Scripts\ninja.exe'
Ok 'Python 3.12, capstone, CMake, Ninja'

$LM = Join-Path $T 'llvm-mingw'
if (!(Test-Path (Join-Path $LM 'bin\x86_64-w64-mingw32-clang.exe'))) {
    $a = Get-Pinned $LM_URL $LM_SHA 'llvm-mingw.zip'
    if (Test-Path $LM) { Remove-Item -Recurse -Force $LM }
    Unpack $a $LM 1
    if (!(Test-Path (Join-Path $LM 'bin\x86_64-w64-mingw32-clang.exe'))) { Fail 'The compiler did not unpack as expected.' }
}
Ok "Compiler: llvm-mingw $LM_VER (clang)"

$FF = Join-Path $Root 'third_party\ffmpeg'
if (!(Test-Path (Join-Path $FF 'lib\libavcodec.dll.a')) -or !(Test-Path (Join-Path $FF 'lib\avcodec.lib'))) {
    $a = Get-Pinned $FF_URL $FF_SHA 'ffmpeg.zip'
    Unpack $a $FF 1
}
Ok 'FFmpeg 8.1 (cutscene video)'

New-Item -ItemType Directory -Force (Join-Path $Root 'tools') | Out-Null
Copy-Item -Force (Join-Path $Root 'third_party\hactool\hactool.exe') (Join-Path $Root 'tools\hactool.exe')
Ok 'hactool (reads your game dump)'

# ---------------------------------------------------------------- configure
$B = Join-Path $Root 'build-cmake'
$cache = Join-Path $B 'CMakeCache.txt'
if ((Test-Path $cache) -and !(Select-String -Path $cache -Pattern 'llvm-mingw' -Quiet)) {
    Say '   (build-cmake was made with another compiler: starting it again)'
    Remove-Item -Force $cache
    if (Test-Path (Join-Path $B 'CMakeFiles')) { Remove-Item -Recurse -Force (Join-Path $B 'CMakeFiles') }
}
function Configure {
    Run $CMake @('-S', $Root, '-B', $B, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$(Fwd $Ninja)",
                 "-DCMAKE_TOOLCHAIN_FILE=$(Fwd (Join-Path $Root 'cmake\llvm-mingw.cmake'))", "-DLLVM_MINGW_ROOT=$(Fwd $LM)",
                 '-DCMAKE_BUILD_TYPE=RelWithDebInfo') 'Configuring the build (CMake)'
}

# ---------------------------------------------------------------- Setup + launcher
Step 'Building XC3 Setup and the launcher'
Configure
Run $CMake @('--build', $B, '--target', 'xc3-setup', 'xc3-launcher') 'Building Setup'
Copy-Item -Force (Join-Path $Root 'tools\hactool.exe') (Join-Path $B 'hactool.exe')
Ok 'build-cmake\xc3-setup.exe, build-cmake\xc3-launcher.exe'
if ($ToolsOnly) { Ok 'Tools built.'; exit 0 }

# ---------------------------------------------------------------- game files
function Game-BuildId {
    $m = Join-Path $Root 'game\exefs\main'
    if (!(Test-Path $m)) { return '' }
    $fs = [IO.File]::OpenRead($m)
    try { $b = New-Object byte[] 0x60; [void]$fs.Read($b, 0, 0x60) } finally { $fs.Close() }
    if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'NSO0') { return '' }
    return (($b[0x40..0x47] | ForEach-Object { $_.ToString('X2') }) -join '')
}
$have = (Test-Path (Join-Path $Root 'game\main.elf')) -and (Test-Path (Join-Path $Root 'game\romfs')) -and ((Game-BuildId) -eq $GAME_BUILD_ID)
if (!$have -or $Setup) {
    Step 'Your game files (XC3 Setup)'
    $free = Free-GB $Root
    if ($free -ge 0 -and $free -lt 30) { Say "   Note: about 30 GB of free space is needed here ($free GB free)." }
    Say '   XC3 Setup opens now. In it:'
    Say '     - your base game (.xci / .nsp), update 2.2.1 (.nsp / .nca) and DLC - or let it find them in Eden / yuzu-based emulators'
    Say '     - prod.keys from your own console (found automatically if an emulator has them)'
    Say "     - keep the install folder it shows ($Root), then press Install"
    Say '   Close Setup when it says the game files are installed: this window continues by itself.'
    Start-Process -FilePath (Join-Path $B 'xc3-setup.exe') -WorkingDirectory $B -Wait
    $have = (Test-Path (Join-Path $Root 'game\main.elf')) -and (Test-Path (Join-Path $Root 'game\romfs'))
    if (!$have) { Fail 'The game files are not installed (game\main.elf / game\romfs missing). Run the installer again to reopen Setup.' }
    $id = Game-BuildId
    if ($id -ne $GAME_BUILD_ID) { Fail "The installed game code is not update 2.2.1 (build id $id, needed $GAME_BUILD_ID): run Setup again with the 2.2.1 update." }
}
Ok 'Game files: game\ (version 2.2.1)'

# ---------------------------------------------------------------- recompile + build
Step 'Recompiling the game code (about 1-2 minutes)'
Run $Py @((Join-Path $Root 'recomp\recompile.py')) 'Recompiling'
Step 'Building xc3.exe (the first time: 10-40 minutes, depending on your CPU; afterwards only what changed)'
Configure
Run $CMake @('--build', $B) 'Building'
Copy-Item -Force (Join-Path $Root 'tools\hactool.exe') (Join-Path $B 'hactool.exe')
if (!(Test-Path (Join-Path $B 'xc3.exe'))) {
    Fail 'xc3.exe is missing after the build. If your antivirus removed it, restore it from quarantine or exclude this folder, then run the installer again.'
}
Ok 'build-cmake\xc3.exe'
if ($BuildOnly) { Ok 'Built.'; exit 0 }

# ---------------------------------------------------------------- bin\ + shortcuts
Step 'Installing (bin\ and shortcuts)'
$Bin = Join-Path $Root 'bin'
New-Item -ItemType Directory -Force $Bin | Out-Null
foreach ($f in @('xc3.exe', 'xc3.pdb')) { if (Test-Path (Join-Path $B $f)) { Copy-Item -Force (Join-Path $B $f) $Bin } }
Get-ChildItem -Path $B -Filter '*.dll' | ForEach-Object { Copy-Item -Force $_.FullName $Bin }
Copy-Item -Force (Join-Path $B 'xc3-launcher.exe') (Join-Path $Bin 'XC3 Launcher.exe')
Copy-Item -Force (Join-Path $Root 'tools\hactool.exe') (Join-Path $Bin 'hactool.exe')
$ini = Join-Path $Bin 'launcher.ini'
if (!(Test-Path $ini)) { Set-Content -Path $ini -Value "[launcher]`r`ngame_dir=$(Join-Path $Root 'game')" -Encoding ASCII }
$launcher = Join-Path $Bin 'XC3 Launcher.exe'
try {
    $sh = New-Object -ComObject WScript.Shell
    foreach ($dir in @([Environment]::GetFolderPath('Desktop'), (Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs'))) {
        $lnk = $sh.CreateShortcut((Join-Path $dir 'Xenoblade Chronicles 3 (recomp).lnk'))
        $lnk.TargetPath = $launcher; $lnk.WorkingDirectory = $Bin
        $lnk.Description = 'Xenoblade Chronicles 3 (recompiled)'; $lnk.Save()
    }
    Ok 'Shortcuts: desktop and Start menu ("Xenoblade Chronicles 3 (recomp)")'
} catch { Say '   (could not make the shortcuts; start bin\XC3 Launcher.exe)' }
Ok "Installed: $launcher"
Write-Host ''
Write-Host 'Done. In game: TAB = settings and mods, Alt+Enter = borderless fullscreen.' -ForegroundColor White
Write-Host 'To update later: run "Install XC3.bat" again. To add DLC: "Install XC3.bat" setup (opens Setup first).'
if (!$NoLaunch) { Start-Process -FilePath $launcher -WorkingDirectory $Bin }
exit 0
