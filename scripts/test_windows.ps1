# scripts/test_windows.ps1
# Automated build, test, and benchmark runner for Windows (WSAPoll + Schannel)

$ErrorActionPreference = "Stop"

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "   BUILDING & TESTING C PROXY ON WINDOWS (WSAPOLL + SCHANNEL)   " -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan

# 1. Ensure build directory exists
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# 2. Compile
Write-Host "[1/3] Compiling zero-dependency Windows build with GCC..." -ForegroundColor Yellow
$srcFiles = @(
    "src/config.c", "src/http.c", "src/http_error.c", "src/logger.c", "src/main.c",
    "src/proxy.c", "src/server.c", "src/tls_schannel.c", "src/upstream.c", "src/url.c",
    "src/poll_wsapoll.c"
)

gcc -Wall -Wextra -Iinclude $srcFiles -lws2_32 -lsecur32 -lcrypt32 -o build/api.exe

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Compilation failed!" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] build/api.exe successfully created!" -ForegroundColor Green

# 3. Launch background process
Write-Host "[2/3] Starting background server on port 17777..." -ForegroundColor Yellow
$proc = Start-Process -FilePath "build\api.exe" -ArgumentList "-p 17777" -PassThru -NoNewWindow
Start-Sleep -Milliseconds 800

try {
    # 4. Run automated test suite
    Write-Host "[3/3] Running automated test suite (node scripts/test_suite.js)..." -ForegroundColor Yellow
    node scripts/test_suite.js
}
finally {
    # 5. Clean teardown
    Write-Host "`nStopping proxy server (PID: $($proc.Id))..." -ForegroundColor Yellow
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "   WINDOWS TEST RUN COMPLETED!                                  " -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan
