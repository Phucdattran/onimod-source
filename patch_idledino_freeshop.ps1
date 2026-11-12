# patch_idledino_freeshop.ps1
# Tự động patch Idle Dino Free Shop:
#   1. classes3.dex: EntitlementInfo.isActive() -> return true
#   2. classes2.dex: LicenseClient.checkLicense() -> return-void (bypass PairIP)
#   3. Fix DEX checksums
#   4. Pull base.apk tu dien thoai, replace DEX, sign, push lai
#
# CHAY: Right-click -> Run with PowerShell (hoac PowerShell: .\patch_idledino_freeshop.ps1)
# YEU CAU: Dien thoai ket noi USB + adb authorized

param([string]$Serial = "cd253267")

$ErrorActionPreference = "Stop"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force 2>$null

$sdk      = "C:\Users\DAT\AppData\Local\Android\Sdk"
$dexdump  = "$sdk\build-tools\35.0.0\dexdump.exe"
$apksign  = "$sdk\build-tools\35.0.0\apksigner.bat"
$adb      = "$sdk\platform-tools\adb.exe"
$z7       = "C:\Program Files\7-Zip\7z.exe"
$keytool  = "C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot\bin\keytool.exe"
$dex3Src  = "C:\Users\DAT\lglloader\idledino\classes3.dex"
$dex2Src  = "C:\Users\DAT\lglloader\idledino\classes2.dex"
$out      = "C:\Users\DAT\lglloader\idledino\patched"
$pkg      = "com.minhnguyen.idledino"

function OK($msg)  { Write-Host "  [OK] $msg" -ForegroundColor Green }
function ERR($msg) { Write-Host "  [!!] $msg" -ForegroundColor Red; exit 1 }
function INF($msg) { Write-Host "  ... $msg" -ForegroundColor Gray }

Write-Host ""
Write-Host "=====================================================" -ForegroundColor Yellow
Write-Host "   IDLE DINO FREE SHOP PATCHER  (adb: $Serial)" -ForegroundColor Yellow
Write-Host "=====================================================" -ForegroundColor Yellow

New-Item -Force -ItemType Directory $out | Out-Null

# ----------------------------------------------------------------
# STEP 1: Patch classes3.dex  -  EntitlementInfo.isActive() = true
# ----------------------------------------------------------------
Write-Host "`n[1/6] Patch classes3.dex (isActive -> true)..." -ForegroundColor Cyan
if (-not (Test-Path $dex3Src)) { ERR "Khong tim thay $dex3Src" }

$b3  = [IO.File]::ReadAllBytes($dex3Src)
$o3  = 0x1DA238
$hex = ($b3[$o3..($o3+5)] | ForEach-Object { $_.ToString("x2") }) -join " "
INF "Bytes tai 0x1DA238: $hex"

# Old: 55 10 16 0a 0f 00  (iget-boolean v0, p0, isActive:Z; return v0)
# New: 12 10 0f 00 00 00  (const/4 v0, 1; return v0; nop)
$b3[$o3]=0x12; $b3[$o3+1]=0x10; $b3[$o3+2]=0x0f
$b3[$o3+3]=0x00; $b3[$o3+4]=0x00; $b3[$o3+5]=0x00
OK "isActive() luon return true"

# ----------------------------------------------------------------
# STEP 2: Tim offset checkLicense trong classes2.dex
# ----------------------------------------------------------------
Write-Host "`n[2/6] Tim checkLicense offset trong classes2.dex..." -ForegroundColor Cyan
if (-not (Test-Path $dex2Src)) { ERR "Khong tim thay $dex2Src" }

$dumpF = "$out\c2dump.txt"
INF "Chay dexdump -d (co the mat vai giay)..."
& $dexdump -d $dex2Src 2>&1 | Set-Content $dumpF -Encoding UTF8

$lines  = [IO.File]::ReadAllLines($dumpF)
$clOff  = $null
$inCL   = $false

for ($i = 0; $i -lt $lines.Count; $i++) {
    $ln = $lines[$i]

    # Bat dau vung checkLicense
    if ($ln -match "name\s*:\s*'checkLicense'" -and
        ($i -gt 0) -and ($lines[$i-3..($i-1)] -join " ") -match "LicenseClient") {
        $inCL = $true
        INF "Tim thay method 'checkLicense' tai dong $i"
        continue
    }
    # Dong dau tien co hex offset -> instruction bat dau tu day
    # Format: "      XXXXXX: XX XX  |0000: opcode"
    if ($inCL -and $ln -match '^\s{4,}([0-9a-fA-F]{4,8}):\s+[0-9a-fA-F]{2}') {
        $clOff = [Convert]::ToInt32($Matches[1], 16)
        INF "checkLicense instruction[0] offset = 0x$($clOff.ToString('X6'))"
        break
    }
    # Het vung method nay
    if ($inCL -and $ln -match "^\s+(Direct|Virtual) method" -and $i -gt 10) { break }
}

# Fallback: tim qua method marker |[offset] ...checkLicense
if ($null -eq $clOff) {
    INF "Fallback: tim marker |[offset]..."
    foreach ($ln in $lines) {
        if ($ln -match '\|\[([0-9a-fA-F]{6,8})\]\s+com\.pairip\S*\.checkLicense:') {
            # code_item offset; instructions bat dau sau header 16 byte
            $clOff = [Convert]::ToInt32($Matches[1], 16) + 16
            INF "code_item offset -> instruction offset = 0x$($clOff.ToString('X6'))"
            break
        }
    }
}

if ($null -eq $clOff) { ERR "Khong tim duoc offset checkLicense! Kiem tra lai file classes2.dex" }

# ----------------------------------------------------------------
# STEP 3: Patch classes2.dex  -  checkLicense -> return-void
# ----------------------------------------------------------------
Write-Host "`n[3/6] Patch classes2.dex (checkLicense -> return-void)..." -ForegroundColor Cyan
$b2 = [IO.File]::ReadAllBytes($dex2Src)
$hexBefore = ($b2[$clOff..($clOff+5)] | ForEach-Object { $_.ToString("x2") }) -join " "
INF "Bytes truoc patch: $hexBefore"

# return-void = 0e 00; sau do nop (00 00) het 33 code units = 66 bytes
$b2[$clOff] = 0x0e; $b2[$clOff+1] = 0x00
for ($i = 2; $i -lt 66; $i++) { $b2[$clOff + $i] = 0x00 }
OK "checkLicense() -> return-void"

# ----------------------------------------------------------------
# STEP 4: Fix DEX checksums (Adler32 + SHA1)
# ----------------------------------------------------------------
Write-Host "`n[4/6] Fix DEX checksums..." -ForegroundColor Cyan

function Fix-Dex([byte[]]$b) {
    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    $h    = $sha1.ComputeHash($b, 0x20, $b.Length - 0x20)
    [Buffer]::BlockCopy($h, 0, $b, 0x0C, 20)

    [uint32]$a = 1; [uint32]$bv = 0
    for ($i = 0x0C; $i -lt $b.Length; $i++) {
        $a  = ($a + $b[$i]) % 65521
        $bv = ($bv + $a)    % 65521
    }
    $c = ($bv -shl 16) -bor $a
    $b[8]  = [byte]($c          -band 0xFF)
    $b[9]  = [byte](($c -shr 8) -band 0xFF)
    $b[10] = [byte](($c -shr 16)-band 0xFF)
    $b[11] = [byte](($c -shr 24)-band 0xFF)
    return $b
}

$b3 = Fix-Dex $b3; [IO.File]::WriteAllBytes("$out\classes3.dex", $b3)
$b2 = Fix-Dex $b2; [IO.File]::WriteAllBytes("$out\classes2.dex", $b2)
OK "Checksums da fix. DEX patched luu vao $out"

# ----------------------------------------------------------------
# STEP 5: Pull base.apk tu dien thoai
# ----------------------------------------------------------------
Write-Host "`n[5/6] Pull APK tu dien thoai va repack..." -ForegroundColor Cyan
INF "Lay duong dan base.apk tren may..."

$rawPath = & $adb -s $Serial shell pm path $pkg 2>&1
$rawPath = "$rawPath"
$baseLine = ($rawPath -split "`n") | Where-Object { $_ -match "base\.apk" } | Select-Object -First 1
if (-not $baseLine) {
    # Thu duong dan khac
    $baseLine = & $adb -s $Serial shell "ls /data/app/*/*/base.apk 2>/dev/null | head -1" 2>&1
    $baseLine = "$baseLine" -replace "package:", ""
}
$baseApkPhone = ($baseLine -replace "package:", "").Trim()
if (-not $baseApkPhone) { ERR "Khong tim duoc base.apk tren dien thoai. Kiem tra ket noi adb." }
INF "base.apk: $baseApkPhone"

$baseOrig = "$out\base_orig.apk"
INF "Dang pull (co the mat 10-20 giay)..."
& $adb -s $Serial pull $baseApkPhone $baseOrig
if ($LASTEXITCODE -ne 0) { ERR "adb pull that bai" }
OK "Pulled: $baseOrig ($([IO.FileInfo]$baseOrig).Length bytes)"

# Replace DEX trong APK
$apkWork = "$out\base_work.apk"
Copy-Item $baseOrig $apkWork -Force
Push-Location $out
INF "Thay the classes2.dex va classes3.dex trong APK..."
& $z7 u $apkWork classes2.dex classes3.dex | Out-Null
# Xoa META-INF (chu ky cu)
& $z7 d $apkWork "META-INF" | Out-Null
Pop-Location
OK "DEX da duoc thay the"

# Tao keystore debug neu chua co
$ks = "$out\debug.keystore"
if (-not (Test-Path $ks)) {
    INF "Tao debug keystore..."
    & $keytool -genkeypair -keystore $ks -alias debug -keyalg RSA -keysize 2048 `
        -validity 10000 -storepass android -keypass android `
        -dname "CN=Debug,O=Debug,C=US" -noprompt 2>&1 | Out-Null
    OK "Keystore tao xong: $ks"
}

# Ky APK
$apkFinal = "$out\IdleDino_FreeShop.apk"
Copy-Item $apkWork $apkFinal -Force
INF "Dang ky APK..."
& $apksign sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true $apkFinal
OK "Signed: $apkFinal"

# Push ve dien thoai
INF "Push APK len /sdcard/Download/..."
& $adb -s $Serial push $apkFinal /sdcard/Download/IdleDino_FreeShop.apk
OK "Da push len /sdcard/Download/IdleDino_FreeShop.apk"

# ----------------------------------------------------------------
# STEP 6: Huong dan cai dat
# ----------------------------------------------------------------
Write-Host "`n[6/6] HUONG DAN CAI DAT:" -ForegroundColor Cyan
Write-Host ""
Write-Host "  CACH 1 (giu du lieu - khuyen dung): MT Manager" -ForegroundColor Yellow
Write-Host "  - Mo MT Manager -> /sdcard/Download/IdleDino_FreeShop.apk"
Write-Host "  - Nhan 'Install' -> chon 'Root Install'" -ForegroundColor Green
Write-Host ""
Write-Host "  CACH 2 (mat du lieu): adb" -ForegroundColor Yellow
Write-Host "  - Chay lenh sau trong PowerShell:" -ForegroundColor Gray
Write-Host "    adb -s $Serial uninstall $pkg"
Write-Host "    adb -s $Serial install `"$apkFinal`""
Write-Host ""
Write-Host "=====================================================" -ForegroundColor Yellow
Write-Host "   XONG! APK da san sang tai:" -ForegroundColor Green
Write-Host "   $apkFinal" -ForegroundColor White
Write-Host "=====================================================" -ForegroundColor Yellow
Write-Host ""
Read-Host "Nhan Enter de thoat"
