<#
.SYNOPSIS
Installs Backup Studio into an Ubuntu 24.04 distribution inside WSL2.

.DESCRIPTION
Backup Studio has no native Windows build: the Qt desktop client, the
command-line client and the archive server are Linux programs. This script
prepares an Ubuntu 24.04 distribution, installs one Debian package inside it,
enables systemd so the packaged service can run, and creates Start Menu and
desktop shortcuts that launch the client through WSLg.

Everything is printed and can be previewed with -WhatIf. The script is safe to
run again: every step checks the current state first.

.PARAMETER PackagePath
The .deb to install. When omitted, the newest matching package under the
repository's delivery directory is used.

.PARAMETER Distro
The WSL distribution to use, created from the Microsoft Store when missing.

.PARAMETER ClientOnly
Install the client package instead of the all-in-one package, so no archive
server is started on this machine.

.PARAMETER NoShortcuts
Do not create Start Menu or desktop shortcuts.

.PARAMETER Remove
Uninstall the package inside the distribution and remove the shortcuts. The
data directory /var/lib/backup-studio is kept.

.PARAMETER Unregister
With -Remove, also delete the whole WSL distribution. Destroys every file in
it, so it asks for confirmation unless -Confirm:$false is passed.

.EXAMPLE
PS> .\install-wsl.ps1 -WhatIf

.EXAMPLE
PS> .\install-wsl.ps1 -PackagePath C:\build\backup-studio_1.8.0_amd64.deb
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string] $PackagePath = "",
    [string] $Distro = "Ubuntu-24.04",
    [string] $UserName = "studio",
    [switch] $ClientOnly,
    [switch] $NoShortcuts,
    [switch] $Remove,
    [switch] $Unregister
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$WslExe = Join-Path $env:SystemRoot "System32\wsl.exe"
$StartMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$Desktop = [Environment]::GetFolderPath("Desktop")

function Invoke-Wsl {
    param(
        [Parameter(Mandatory = $true)][string[]] $ArgumentList,
        [string] $User = "root"
    )
    $arguments = @("-d", $Distro, "-u", $User, "--") + $ArgumentList
    & $WslExe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "wsl $($arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Invoke-WslShell {
    param(
        [Parameter(Mandatory = $true)][string] $Script,
        [string] $User = "root"
    )
    # bash -lc keeps the quoting predictable for multi-line shell fragments.
    Invoke-Wsl -User $User -ArgumentList @("bash", "-lc", $Script)
}

function Get-WslDistros {
    # wsl.exe writes UTF-16; PowerShell 5.1 hands it back with NUL bytes.
    $raw = & $WslExe -l -q 2>$null
    if ($LASTEXITCODE -ne 0) {
        return @()
    }
    return @($raw | ForEach-Object { ($_ -replace "`0", "").Trim() } |
        Where-Object { $_ })
}

function Find-Package {
    $flavour = if ($ClientOnly) { "backup-studio-client" } else { "backup-studio" }
    if ($PackagePath) {
        if (-not (Test-Path -LiteralPath $PackagePath)) {
            throw "package not found: $PackagePath"
        }
        return (Resolve-Path -LiteralPath $PackagePath).Path
    }
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $delivery = Join-Path (Split-Path -Parent $repo) "backup_system_delivery"
    $candidates = @()
    foreach ($base in @($PSScriptRoot, $delivery)) {
        if (Test-Path -LiteralPath $base) {
            $candidates += Get-ChildItem -Path $base -Filter "${flavour}_*.deb" `
                -Recurse -File -ErrorAction SilentlyContinue
        }
    }
    if ($candidates.Count -eq 0) {
        throw ("no ${flavour}_*.deb found; build one with " +
            "Utils/package-wsl.sh or pass -PackagePath")
    }
    return ($candidates | Sort-Object LastWriteTime -Descending |
            Select-Object -First 1).FullName
}

function New-Shortcut {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $Target,
        [Parameter(Mandatory = $true)][string] $Arguments,
        [string] $Description = ""
    )
    if (-not $PSCmdlet.ShouldProcess($Path, "create shortcut")) {
        return
    }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $Target
    $shortcut.Arguments = $Arguments
    $shortcut.Description = $Description
    $shortcut.Save()
}

function Install-Shortcuts {
    # wslg.exe moved from the Store package to the MSI package; fall back to
    # wsl.exe, which also reaches WSLg on current WSL releases.
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\wslg.exe"),
        (Join-Path $env:ProgramFiles "WSL\wslg.exe")
    )
    $wslg = $candidates | Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    $target = if ($wslg) { $wslg } else { $WslExe }
    if (-not $wslg) {
        Write-Warning ("wslg.exe was not found; the client needs WSLg from " +
            "Windows 11 or the current WSL package. The shortcut uses wsl.exe " +
            "instead and may not open a window.")
    }
    $directories = @($StartMenu)
    if ($Desktop) {
        $directories += $Desktop
    }
    foreach ($directory in $directories) {
        New-Shortcut -Path (Join-Path $directory "Backup Studio.lnk") `
            -Target $target -Arguments "-d $Distro -- /usr/bin/backup-gui" `
            -Description "Backup Studio desktop client (WSL2)"
    }
}

function Remove-Shortcuts {
    $directories = @($StartMenu)
    if ($Desktop) {
        $directories += $Desktop
    }
    foreach ($directory in $directories) {
        $path = Join-Path $directory "Backup Studio.lnk"
        if (Test-Path -LiteralPath $path) {
            if ($PSCmdlet.ShouldProcess($path, "remove shortcut")) {
                Remove-Item -LiteralPath $path -Force
            }
        }
    }
}

# --- prerequisites -----------------------------------------------------------

if (-not (Test-Path -LiteralPath $WslExe)) {
    throw "wsl.exe was not found; install WSL2 first: wsl --install"
}
if ((Get-WslDistros) -notcontains $Distro) {
    if ($Remove) {
        Write-Host "$Distro is not installed; nothing to remove."
        Remove-Shortcuts
        return
    }
    if ($PSCmdlet.ShouldProcess($Distro, "install the WSL distribution")) {
        Write-Host "installing $Distro from the Microsoft Store"
        & $WslExe --install -d $Distro --no-launch
        if ($LASTEXITCODE -ne 0) {
            throw ("wsl --install failed. Run it from an elevated prompt, or " +
                "install the distribution manually and run this script again.")
        }
    }
}

if ($Remove) {
    if ($PSCmdlet.ShouldProcess($Distro, "uninstall the Backup Studio package")) {
        Invoke-WslShell @"
set -e
if command -v dpkg >/dev/null && dpkg -s backup-studio >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get remove -y backup-studio
fi
if command -v dpkg >/dev/null && dpkg -s backup-studio-client >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get remove -y backup-studio-client
fi
if command -v dpkg >/dev/null && dpkg -s backup-studio-server >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get remove -y backup-studio-server
fi
echo 'kept /var/lib/backup-studio; delete it manually to remove the backups'
"@
    }
    Remove-Shortcuts
    if ($Unregister) {
        if ($PSCmdlet.ShouldProcess($Distro, "delete the whole distribution")) {
            & $WslExe --unregister $Distro
            if ($LASTEXITCODE -ne 0) {
                throw "wsl --unregister $Distro failed"
            }
        }
    }
    return
}

# --- install -----------------------------------------------------------------

$package = Find-Package
Write-Host "installing $(Split-Path -Leaf $package) into $Distro"

# The distribution may never have been started, in which case it has no
# regular user yet; the desktop client must not run as root.
if ($PSCmdlet.ShouldProcess($Distro, "prepare the user and systemd")) {
    Invoke-WslShell @"
set -e
if ! id -u $UserName >/dev/null 2>&1; then
    adduser --disabled-password --gecos '' $UserName
    usermod -aG sudo $UserName
fi
if ! grep -q '^\[user\]' /etc/wsl.conf 2>/dev/null; then
    printf '\n[user]\ndefault=$UserName\n' >> /etc/wsl.conf
fi
# The packaged server is managed by systemd, which WSL2 keeps disabled until
# /etc/wsl.conf asks for it.
if ! grep -q '^systemd=true' /etc/wsl.conf 2>/dev/null; then
    printf '\n[boot]\nsystemd=true\n' >> /etc/wsl.conf
fi
"@
}

if ($PSCmdlet.ShouldProcess($package, "install inside $Distro")) {
    $wslPath = (Invoke-Wsl -ArgumentList @("wslpath", "-u", $package) |
        Select-Object -Last 1).Trim()
    Invoke-WslShell @"
set -e
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq '$wslPath'
"@
}

# systemd only starts after the distribution is restarted.
if ($PSCmdlet.ShouldProcess($Distro, "restart the distribution")) {
    & $WslExe -t $Distro | Out-Null
}
if (-not $ClientOnly) {
    if ($PSCmdlet.ShouldProcess($Distro, "check the packaged service")) {
        Invoke-WslShell @"
systemctl daemon-reload || true
systemctl enable --now backup-studio.service || true
systemctl is-active backup-studio.service || true
"@
    }
}
if (-not $NoShortcuts) {
    Install-Shortcuts
}

if ($PSCmdlet.ShouldProcess($Distro, "verify the installed client")) {
    Invoke-WslShell -User $UserName "backup-cli --version"
}
if ($WhatIfPreference) {
    Write-Host ""
    Write-Host "Dry run complete: no change was made."
    return
}

Write-Host ""
Write-Host "Backup Studio is installed in $Distro."
Write-Host "  desktop client: Start Menu -> Backup Studio"
Write-Host "  command line:   wsl -d $Distro -u $UserName -- backup-cli --help"
Write-Host "  configuration:  wsl -d $Distro -u root -- nano /etc/backup-studio/databackup.conf"
Write-Host "  uninstall:      .\install-wsl.ps1 -Remove"
