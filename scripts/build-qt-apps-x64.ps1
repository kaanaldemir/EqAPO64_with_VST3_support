param(
	[string]$Configuration = "Release",
	[string]$VisualStudioEdition = "Community",
	[string]$QtRoot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $root "third_party"
$triplet = "x64-windows"

if ($QtRoot -eq "") {
	$qtCandidates = Get-ChildItem -Path (Join-Path $thirdParty "Qt") -Filter qmake.exe -Recurse -ErrorAction SilentlyContinue |
		Where-Object { $_.FullName -match "\\msvc2022_64\\bin\\qmake.exe$" } |
		Sort-Object FullName
	if ($qtCandidates.Count -eq 0) {
		throw "Qt qmake.exe not found under third_party\Qt. Run scripts\bootstrap-third-party.ps1 -WithQt first."
	}
	$QtRoot = Split-Path -Parent (Split-Path -Parent $qtCandidates[0].FullName)
}

$qmake = Join-Path $QtRoot "bin\qmake.exe"
$lrelease = Join-Path $QtRoot "bin\lrelease.exe"
if (!(Test-Path -LiteralPath $qmake)) {
	throw "qmake.exe not found: $qmake"
}

$paths = @{
	LIBSNDFILE_INCLUDE = Join-Path $thirdParty "vcpkg_installed\$triplet\include"
	LIBSNDFILE_LIB = Join-Path $thirdParty "vcpkg_installed\$triplet\lib"
	FFTW_INCLUDE = Join-Path $thirdParty "vcpkg_installed\$triplet\include"
	FFTW_LIB = Join-Path $thirdParty "vcpkg_installed\$triplet\lib"
	MUPARSERX_INCLUDE = Join-Path $thirdParty "muparserx\parser"
	MUPARSERX_LIB = Join-Path $thirdParty "muparserx\build\x64\$Configuration"
}

foreach ($path in $paths.GetEnumerator()) {
	if (!(Test-Path -LiteralPath $path.Value)) {
		throw "Missing dependency path $($path.Key): $($path.Value)"
	}
}

$vsDevCmd = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\$VisualStudioEdition\Common7\Tools\VsDevCmd.bat"
if (!(Test-Path -LiteralPath $vsDevCmd)) {
	throw "Visual Studio 2022 $VisualStudioEdition VsDevCmd.bat not found: $vsDevCmd"
}

$apps = @(
	@{ Name = "Editor"; Project = Join-Path $root "Editor\Editor.pro" },
	@{ Name = "DeviceSelector"; Project = Join-Path $root "DeviceSelector\DeviceSelector.pro" },
	@{ Name = "UpdateChecker"; Project = Join-Path $root "UpdateChecker\UpdateChecker.pro" }
)

$outDir = Join-Path $root "x64\$Configuration"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

foreach ($app in $apps) {
	$buildDir = Join-Path $root "_build\$($app.Name)-x64"
	New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

	if (Test-Path -LiteralPath $lrelease) {
		& $lrelease $app.Project
		if ($LASTEXITCODE -ne 0) {
			throw "lrelease failed for $($app.Name) with exit code $LASTEXITCODE"
		}
	}

	$cmdParts = @(
		"call `"$vsDevCmd`" -arch=x64 -host_arch=x64",
		"set `"LIBSNDFILE_INCLUDE=$($paths.LIBSNDFILE_INCLUDE)`"",
		"set `"LIBSNDFILE_LIB=$($paths.LIBSNDFILE_LIB)`"",
		"set `"FFTW_INCLUDE=$($paths.FFTW_INCLUDE)`"",
		"set `"FFTW_LIB=$($paths.FFTW_LIB)`"",
		"set `"MUPARSERX_INCLUDE=$($paths.MUPARSERX_INCLUDE)`"",
		"set `"MUPARSERX_LIB=$($paths.MUPARSERX_LIB)`"",
		"cd /d `"$buildDir`"",
		"`"$qmake`" `"$($app.Project)`" -spec win32-msvc CONFIG+=release CONFIG-=debug",
		"nmake"
	)

	cmd /c ($cmdParts -join " && ")
	if ($LASTEXITCODE -ne 0) {
		throw "$($app.Name) build failed with exit code $LASTEXITCODE"
	}

	$exe = Get-ChildItem -LiteralPath $buildDir -Filter "$($app.Name).exe" -Recurse |
		Where-Object { $_.FullName -match "\\release\\" } |
		Select-Object -First 1
	if ($null -eq $exe) {
		throw "$($app.Name).exe was not produced under $buildDir"
	}

	Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $outDir "$($app.Name).exe") -Force
}

Write-Host "Qt applications are ready in $outDir."
