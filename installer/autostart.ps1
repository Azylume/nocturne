# Starts Nocturne at logon under its supervisor, as a scheduled task in the user's desktop session: the Windows
# counterpart of tools/autostart.sh. The installer runs it from the install folder; it also works by hand, as admin:
#
#   powershell -ExecutionPolicy Bypass -File "C:\Program Files\Nocturne\autostart.ps1" install|uninstall|status
#
# install registers the task "Nocturne" for the account that runs it; the supervisor starts at that account's next
# logon (or with `Start-ScheduledTask Nocturne`) with its default folders under %LOCALAPPDATA%\Nocturne.
param([Parameter(Mandatory)][ValidateSet('install', 'uninstall', 'status')][string]$Action)
$ErrorActionPreference = 'Stop'
$TaskName = 'Nocturne'

function Install-Autostart
{
    $supervisor = Join-Path $PSScriptRoot 'nocturne-supervisor.exe'
    if (-not (Test-Path $supervisor)) { throw "no $supervisor" }
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $action = New-ScheduledTaskAction -Execute $supervisor -WorkingDirectory $PSScriptRoot
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    # Interactive: the engine's outputs need the desktop. A default task runs below normal priority, refuses batteries
    # and is stopped after three days; a show is none of those.
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit 0 `
        -Priority 4 -MultipleInstances IgnoreNew -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal `
        -Settings $settings -Force | Out-Null
    "installed the task $TaskName for $user; the supervisor starts at every logon"
}

function Uninstall-Autostart
{
    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if (-not $task) { return }
    Stop-ScheduledTask -TaskName $TaskName
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    # Stopping returns before the supervisor and its children have exited; the uninstaller deletes their files next.
    Get-Process -Name 'nocturne-supervisor', 'nocturne', 'nocturne-desk' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($PSScriptRoot) } |
        Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
    "uninstalled; the engine no longer starts at logon"
}

switch ($Action) {
    'install' { Install-Autostart }
    'uninstall' { Uninstall-Autostart }
    'status' { Get-ScheduledTask -TaskName $TaskName | Get-ScheduledTaskInfo | Format-List LastRunTime, LastTaskResult }
}
