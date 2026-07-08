$ErrorActionPreference = "Stop"

# Install hooks only for this repository by using local Git config.
$RepoRootOutput = @(& git -c "safe.directory=*" rev-parse --show-toplevel)
if ($LASTEXITCODE -ne 0 -or $RepoRootOutput.Count -eq 0) {
    throw "Cannot find the Git repository root."
}

$RepoRoot = [string]$RepoRootOutput[0]
$GitSafeDirectory = $RepoRoot.Replace("\", "/")

# core.hooksPath makes Git run versioned hooks from .githooks instead of .git/hooks.
& git -C $RepoRoot -c "safe.directory=$GitSafeDirectory" config core.hooksPath .githooks
if ($LASTEXITCODE -ne 0) {
    throw "Failed to configure core.hooksPath."
}

Write-Host "Installed VisionMonitor Git hooks from .githooks." -ForegroundColor Green
