$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Get-Command gcc -ErrorAction SilentlyContinue) -or -not (Get-Command windres -ErrorAction SilentlyContinue)) {
    Write-Error "gcc y windres tienen que estar en PATH. Instala MinGW-w64 y vuelve a ejecutar."
    exit 1
}
Set-Location $root
& windres -O coff -i src/hwstatz.rc -o src/hwstatz.res
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& gcc -O2 -s -Wall -Wextra -mwindows -ffunction-sections -fdata-sections -finput-charset=UTF-8 -fexec-charset=UTF-8 "-Wl,--gc-sections" -o hwstatz.exe src/hwstatz.c src/hwstatz.res -lshell32 -luser32 -lgdi32
$code = $LASTEXITCODE
Remove-Item -Force src/hwstatz.res -ErrorAction SilentlyContinue
if ($code -ne 0) { exit $code }
$src = Get-Content -Raw "src\hwstatz.c"
$exeText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$root\hwstatz.exe"))
if ($src.Contains("ioctl_write") -or $exeText.Contains("ioctl_write_msr") -or $exeText.Contains("ioctl_write_smn")) {
    Write-Error "aparecio un ioctl de escritura en el fuente o en el exe"
    exit 1
}
exit 0
