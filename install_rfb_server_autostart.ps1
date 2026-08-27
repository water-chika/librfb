<#
.SYNOPSIS
    Install / remove an autostart entry for rfb_server.

.DESCRIPTION
    rfb_server captures the desktop of the session it runs in, and anything
    started at boot lands in session 0, which has no visible desktop. Two
    mechanisms are offered:

    -AtBoot (the default) - a scheduled task with an "At startup" trigger,
        running as LocalSystem, whose action is
        "rfb_server --console-launcher ...". That launcher stays in session 0
        and pushes the real server into whichever session is attached to the
        console; that session exists from boot onwards, showing the logon
        screen. The desktop is therefore served before anyone logs in, and
        stays served across logon and logoff.

    -AtLogon - a per-user logon item instead:
        %LOCALAPPDATA%\librfb\rfb_server_start.cmd   - the command line
        <Startup folder>\rfb_server.vbs              - runs the .cmd hidden
        Nothing starts until the user signs in. Use this when the machine has
        no usable task scheduler, or when the server should only run while
        this user is logged on.

    (A scheduled task created with "schtasks /IT" would be the obvious way to
    reach the interactive session directly, but it fails with result 1 on some
    machines, hence the two routes above.)

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install_rfb_server_autostart.ps1 `
        -Port 5901 -Listen 127.0.0.1 -Name water-coffee-rfb -StartNow

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install_rfb_server_autostart.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    # Full path to rfb_server.exe.  Auto-detected next to this script if omitted.
    [string] $ServerPath,

    [ValidateRange(1, 65535)]
    [int] $Port = 5900,

    # Bind address, e.g. 127.0.0.1 to force access through an SSH tunnel.
    [string] $Listen = '',

    # VNC authentication password (max 8 characters, protocol limit).
    [string] $Password = '',

    # Desktop name shown by the viewer.
    [string] $Name = '',

    [ValidateRange(1, 120)]
    [int] $Fps = 20,

    [switch] $AllMonitors,
    [switch] $NoCursor,
    [switch] $ViewOnly,

    # Start at boot, before any logon, via a LocalSystem scheduled task.
    [switch] $AtBoot,

    # Start at this user's logon instead, via a Startup-folder item.
    [switch] $AtLogon,

    # Name of the scheduled task created by -AtBoot.
    [string] $TaskName = 'rfb_server',

    # Also start the server right now, without waiting for the next boot/logon.
    [switch] $StartNow,

    # Remove the autostart entry and stop a running server.
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'

if ($AtBoot -and $AtLogon) {
    throw 'Choose either -AtBoot or -AtLogon, not both.'
}
# "Start on boot" is the point of this script, so -AtBoot is the default.
if (-not $AtLogon) { $AtBoot = $true }

$stateDir    = Join-Path $env:LOCALAPPDATA 'librfb'
$launcherCmd = Join-Path $stateDir 'rfb_server_start.cmd'
$startupDir  = [Environment]::GetFolderPath('Startup')
$startupVbs  = Join-Path $startupDir 'rfb_server.vbs'
# The boot task runs as LocalSystem, which has no user profile to log into.
$bootStateDir = Join-Path $env:ProgramData 'librfb'
$bootLogPath  = Join-Path $bootStateDir 'rfb_server.log'
$logonLogPath = Join-Path $stateDir 'rfb_server.log'

function Resolve-ServerPath {
    if ($ServerPath) {
        if (-not (Test-Path -LiteralPath $ServerPath)) {
            throw "rfb_server.exe not found at '$ServerPath'."
        }
        return (Resolve-Path -LiteralPath $ServerPath).Path
    }

    $candidates = @(
        'build\librfb\Release\rfb_server.exe'
        'build\librfb\RelWithDebInfo\rfb_server.exe'
        'build\librfb\Debug\rfb_server.exe'
        'build\librfb\rfb_server.exe'
        'librfb\Release\rfb_server.exe'
        'Release\rfb_server.exe'
        'rfb_server.exe'
    )
    # Walk up from the script directory: librfb/ -> project root -> build root.
    $roots = @()
    $dir = $PSScriptRoot
    for ($i = 0; $i -lt 4 -and $dir; $i++) {
        $roots += $dir
        $dir = Split-Path -Parent $dir
    }

    foreach ($root in $roots) {
        foreach ($candidate in $candidates) {
            $probe = Join-Path $root $candidate
            if (Test-Path -LiteralPath $probe) {
                return (Resolve-Path -LiteralPath $probe).Path
            }
        }
    }

    throw ("Could not auto-detect rfb_server.exe near '$PSScriptRoot'. " +
           'Build it first, or pass -ServerPath.')
}

function Remove-Autostart {
    foreach ($path in @($startupVbs, $launcherCmd)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
            Write-Host "removed $path"
        }
    }
    # schtasks writes to stderr when the task does not exist, which PowerShell
    # would surface as a scary NativeCommandError; absence is not an error here.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    & schtasks.exe /query /TN $TaskName 2>&1 | Out-Null
    $exists = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $previous
    if ($exists) {
        & schtasks.exe /end /TN $TaskName 2>&1 | Out-Null
        & schtasks.exe /delete /TN $TaskName /F | Out-Null
        Write-Host "removed scheduled task $TaskName"
    }
}

function Stop-Server {
    Get-Process -Name 'rfb_server' -ErrorAction SilentlyContinue |
        ForEach-Object {
            Write-Host "stopping rfb_server.exe (pid $($_.Id))"
            Stop-Process -Id $_.Id -Force
        }
}

if ($Uninstall) {
    Remove-Autostart
    Stop-Server
    Write-Host 'rfb_server autostart uninstalled.'
    return
}

if ($Password -and $Password.Length -gt 8) {
    throw '-Password is limited to 8 characters by the VNC protocol.'
}

$exe = Resolve-ServerPath

$serverArgs = @('--port', $Port, '--fps', $Fps)
if ($Listen)      { $serverArgs += @('--listen', $Listen) }
if ($Password)    { $serverArgs += @('--password', $Password) }
if ($Name)        { $serverArgs += @('--name', $Name) }
if ($AllMonitors) { $serverArgs += '--all-monitors' }
if ($NoCursor)    { $serverArgs += '--no-cursor' }
if ($ViewOnly)    { $serverArgs += '--view-only' }

$quoted = $serverArgs | ForEach-Object {
    if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" }
}
$argumentText = $quoted -join ' '

Write-Host "server    : $exe"
Write-Host "arguments : $argumentText"

if ($AtBoot) {
    New-Item -ItemType Directory -Path $bootStateDir -Force | Out-Null

    # cmd wraps the launcher purely to get the log redirect; the launcher and
    # its worker are both long-lived, so this cmd lives as long as the task.
    $action = 'cmd.exe /c ""{0}" --console-launcher {1} > "{2}" 2>&1"' -f `
        $exe, $argumentText, $bootLogPath

    # /RU SYSTEM is what makes the launcher able to move a token into the
    # console session; /RL HIGHEST avoids a filtered admin token.
    & schtasks.exe /create /TN $TaskName /TR $action /SC ONSTART `
        /RU 'SYSTEM' /RL HIGHEST /F | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "schtasks /create failed with exit code $LASTEXITCODE"
    }

    Write-Host "task      : $TaskName (At startup, as SYSTEM)"
    Write-Host "log       : $bootLogPath"
    Write-Host 'installed - rfb_server will start at boot, before any logon.'

    if ($StartNow) {
        & schtasks.exe /run /TN $TaskName | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "schtasks /run failed with exit code $LASTEXITCODE"
        }
        Write-Host 'started now via the same task.'
    }
    return
}

# -AtLogon: a hidden per-user Startup item.
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null

$cmdBody = @"
@echo off
rem Generated by install_rfb_server_autostart.ps1 - do not edit by hand.
rem One instance only: bail out if rfb_server.exe is already running.
tasklist /fi "IMAGENAME eq rfb_server.exe" | find /i "rfb_server.exe" >nul
if not errorlevel 1 exit /b 0
"$exe" $argumentText > "$logonLogPath" 2>&1
"@
Set-Content -LiteralPath $launcherCmd -Value $cmdBody -Encoding ASCII

$vbsBody = @"
' Generated by install_rfb_server_autostart.ps1 - do not edit by hand.
' Runs the launcher with no console window: a visible window on the user's
' desktop would take the server down with it when closed.
CreateObject("WScript.Shell").Run """$launcherCmd""", 0, False
"@
Set-Content -LiteralPath $startupVbs -Value $vbsBody -Encoding ASCII

Write-Host "launcher  : $launcherCmd"
Write-Host "autostart : $startupVbs"
Write-Host "log       : $logonLogPath"
Write-Host 'installed - rfb_server will start at the next interactive logon.'

if (-not $StartNow) { return }

$inInteractiveSession = ($env:SESSIONNAME -and $env:SESSIONNAME -ne 'Services')

if ($inInteractiveSession) {
    Start-Process -FilePath 'wscript.exe' `
                  -ArgumentList @("`"$startupVbs`"") `
                  -WindowStyle Hidden
    Write-Host 'started in the current interactive session.'
    return
}

# Session 0 (ssh / service context): the desktop is invisible there, so hand
# the launcher to the console session through PsExec instead.
$psexec = @(
    'C:\PSTools\PsExec64.exe'
    'C:\PSTools\PsExec.exe'
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if (-not $psexec) {
    $psexec = (Get-Command 'PsExec64.exe', 'PsExec.exe' -ErrorAction SilentlyContinue |
               Select-Object -First 1).Source
}

if (-not $psexec) {
    Write-Warning ('Running in session 0 and PsExec was not found, so the ' +
                   'server was not started now. Log on to the console, or use ' +
                   '-AtBoot, which needs no PsExec.')
    return
}

$consoleId = (Get-Process -Name 'explorer' -ErrorAction SilentlyContinue |
              Select-Object -First 1).SessionId
if (-not $consoleId) { $consoleId = 1 }

# -s (LocalSystem) matters: a PsExec launch under the calling user's token
# lands on WinSta0\Default but is still denied access to the input desktop, so
# every BitBlt fails and viewers see black. Launch through the .vbs rather than
# the .cmd so no console window appears on the user's desktop.
& $psexec -accepteula -nobanner -s -i $consoleId -d `
    'wscript.exe' "`"$startupVbs`"" | Out-Null
Write-Host "started hidden in session $consoleId as LocalSystem via $psexec."
