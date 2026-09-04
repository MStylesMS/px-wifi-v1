# Serve the embedded admin UI for local theme/IA work (demo mocks, no ESP).
# Open http://127.0.0.1:8090/index.html
# 8080 is often in Windows excluded-port ranges (WinError 10013).
$ErrorActionPreference = "Stop"
$port = 8090
$webui = Join-Path $PSScriptRoot "..\main\webui"
Set-Location $webui
Write-Host "Serving $((Resolve-Path $webui).Path) on http://127.0.0.1:$port/"
Write-Host "Demo mocks auto-enable on localhost. Ctrl+C to stop."
python -m http.server $port --bind 127.0.0.1
