# Removes OpenCamHub. Run as administrator. Your settings and LUTs in %ProgramData%\OpenCamHub are kept
# unless you pass -RemoveSettings.
param([switch]$RemoveSettings)
$ErrorActionPreference = 'Continue'
$dest = "$env:ProgramFiles\OpenCamHub"

Get-Process OpenCamHub -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path "$dest\opencamhub-setup.exe") { & "$dest\opencamhub-setup.exe" --unregister }
foreach ($svc in 'FrameServer', 'FrameServerMonitor') {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -eq 'Running') { Stop-Service $svc -Force }
}
Remove-Item 'HKLM:\SOFTWARE\Classes\CLSID\{5B2B95F8-965B-4FC7-8A72-0ED88649C542}' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\OpenCamHub.lnk" -Force -ErrorAction SilentlyContinue
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name OpenCamHub -ErrorAction SilentlyContinue
if ($RemoveSettings) { Remove-Item "$env:ProgramData\OpenCamHub" -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host 'OpenCamHub removed.'
