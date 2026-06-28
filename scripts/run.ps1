<#
    Build (if needed) and launch the full stack on Windows:
    the C++ matching engine, the Rust gateway, and the web console.

    Usage:  powershell -ExecutionPolicy Bypass -File scripts\run.ps1
            scripts\run.ps1 -EnginePort 9001 -GatewayPort 8080
#>
param(
    [int]$EnginePort = 9001,
    [int]$GatewayPort = 8080
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$cargo = "cargo"
if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
    $cargo = Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe"
}

Write-Host "==> Building matching engine (C++)" -ForegroundColor Cyan
if (-not (Test-Path "cpp/build")) { cmake -S cpp -B cpp/build -G Ninja }
cmake --build cpp/build

Write-Host "==> Building gateway (Rust)" -ForegroundColor Cyan
& $cargo build --release -p gateway-server

$engineExe  = "cpp/build/server/hme_engine_server.exe"
$gatewayExe = "target/release/hme-gateway.exe"

Write-Host "==> Starting engine on 127.0.0.1:$EnginePort" -ForegroundColor Green
$engine = Start-Process -PassThru -FilePath $engineExe -ArgumentList "--host","127.0.0.1","--port","$EnginePort"
Start-Sleep -Seconds 1

Write-Host "==> Starting gateway on 127.0.0.1:$GatewayPort" -ForegroundColor Green
$gateway = Start-Process -PassThru -FilePath $gatewayExe -ArgumentList "--listen","127.0.0.1:$GatewayPort","--engine","127.0.0.1:$EnginePort","--web","./web"
Start-Sleep -Seconds 2

Start-Process "http://localhost:$GatewayPort"
Write-Host ""
Write-Host "Console:  http://localhost:$GatewayPort" -ForegroundColor Yellow
Write-Host "Engine PID $($engine.Id), Gateway PID $($gateway.Id)."
Write-Host "Press Enter to stop both."
[void](Read-Host)

Stop-Process -Id $gateway.Id -ErrorAction SilentlyContinue
Stop-Process -Id $engine.Id  -ErrorAction SilentlyContinue
Write-Host "Stopped."
