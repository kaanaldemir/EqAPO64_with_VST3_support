param(
	[string]$Flavor = "avx512",
	[string]$Commit = "local"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$headerPath = Join-Path $root "UpdateChecker\BuildMetadata.h"

if ($Commit.Length -gt 12) {
	$Commit = $Commit.Substring(0, 12)
}

$safeFlavor = $Flavor -replace '[^A-Za-z0-9_.-]', '-'
$safeCommit = $Commit -replace '[^A-Za-z0-9_.-]', '-'

$content = @(
	"#pragma once",
	"",
	"#define EAPO_BUILD_FLAVOR `"$safeFlavor`"",
	"#define EAPO_BUILD_COMMIT `"$safeCommit`""
)

Set-Content -LiteralPath $headerPath -Value $content -Encoding ASCII
