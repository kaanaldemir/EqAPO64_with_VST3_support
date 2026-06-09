param(
	[string]$Configuration = "Release",
	[string]$VisualStudioEdition = "Community",
	[string]$Makensis = "",
	[ValidateSet("AdvancedVectorExtensions2", "AdvancedVectorExtensions512")]
	[string]$InstructionSet = "AdvancedVectorExtensions2",
	[string]$InstallerSuffix = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $root "scripts\bootstrap-third-party.ps1") -Configuration $Configuration -WithQt -WithNsis
$archFlag = if ($InstructionSet -eq "AdvancedVectorExtensions512") { "/arch:AVX512" } else { "/arch:AVX2" }
if ($InstallerSuffix -eq "" -and $InstructionSet -eq "AdvancedVectorExtensions512") {
	$InstallerSuffix = "-AVX512"
}

& (Join-Path $root "build-local-x64.ps1") -Configuration $Configuration -VisualStudioEdition $VisualStudioEdition -InstructionSet $InstructionSet
& (Join-Path $root "scripts\build-qt-apps-x64.ps1") -Configuration $Configuration -VisualStudioEdition $VisualStudioEdition -ArchFlag $archFlag
& (Join-Path $root "scripts\stage-installer-x64.ps1") -Configuration $Configuration

if ($Makensis -eq "") {
	$localMakensis = Join-Path $root "third_party\nsis-3.11\makensis.exe"
	if (Test-Path -LiteralPath $localMakensis) {
		$Makensis = $localMakensis
	}
}

if ($Makensis -eq "") {
	$makensisCommand = Get-Command makensis.exe -ErrorAction SilentlyContinue
	if ($null -ne $makensisCommand) {
		$Makensis = $makensisCommand.Source
	}
}

if ($Makensis -eq "") {
	$candidates = @(
		Join-Path ${env:ProgramFiles} "NSIS\makensis.exe",
		Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"
	)
	$found = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
	if ($null -ne $found) {
		$Makensis = $found
	}
}

if ($Makensis -eq "") {
	throw "makensis.exe was not found. Install NSIS or pass -Makensis <path>."
}

Push-Location (Join-Path $root "Setup")
try {
	$makensisArgs = @()
	if ($InstallerSuffix -ne "") {
		$makensisArgs += "/DOUTFILE_SUFFIX=$InstallerSuffix"
	}
	$makensisArgs += ".\Setup64.nsi"
	& $Makensis @makensisArgs
	if ($LASTEXITCODE -ne 0) {
		throw "makensis failed with exit code $LASTEXITCODE"
	}
}
finally {
	Pop-Location
}

Write-Host "Installer build finished."
