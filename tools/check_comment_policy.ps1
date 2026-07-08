param(
    [ValidateSet("working", "staged")]
    [string]$Mode = "working"
)

$ErrorActionPreference = "Stop"

# Resolve the real repository root from the caller's current directory. Codex
# may execute this script through a sandbox path, so PSScriptRoot is not enough.
$StartDirectory = (Get-Location).Path
$repoRootOutput = @(& git -C $StartDirectory -c "safe.directory=*" rev-parse --show-toplevel)
if ($LASTEXITCODE -ne 0 -or $repoRootOutput.Count -eq 0) {
    throw "Cannot find the Git repository root from $StartDirectory"
}

$RepoRoot = [string]$repoRootOutput[0]
$GitSafeDirectory = $RepoRoot.Replace("\", "/")
Set-Location $RepoRoot

function Invoke-Git {
    param(
        [string[]]$Arguments,
        [switch]$AllowFailure
    )

    # The workspace can be owned by a different Windows SID when Codex runs it,
    # so every Git call carries a repository-scoped safe.directory override.
    if ($AllowFailure) {
        $output = & git -C $RepoRoot -c "safe.directory=$GitSafeDirectory" @Arguments 2>$null
    } else {
        $output = & git -C $RepoRoot -c "safe.directory=$GitSafeDirectory" @Arguments
    }
    if ($LASTEXITCODE -ne 0 -and -not $AllowFailure) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }

    return @($output)
}

function Test-CodeFile {
    param([string]$Path)

    # Check implementation files only; docs and assets do not need code comments.
    $fileName = [System.IO.Path]::GetFileName($Path)
    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    $codeExtensions = @(".cpp", ".h", ".hpp", ".c", ".cc", ".cxx", ".cmake", ".ps1", ".bat", ".cmd")

    return $fileName -eq "CMakeLists.txt" -or $codeExtensions.Contains($extension)
}

function Get-ChangedCodeFiles {
    # "working" checks unstaged and new files; "staged" checks the commit candidate.
    if ($Mode -eq "staged") {
        $changed = Invoke-Git @("diff", "--cached", "--name-only", "--diff-filter=ACMR")
        return @($changed | Where-Object { Test-CodeFile $_ })
    }

    $tracked = Invoke-Git @("diff", "--name-only", "--diff-filter=ACMR")
    $untracked = Invoke-Git @("ls-files", "--others", "--exclude-standard")
    return @(($tracked + $untracked) | Where-Object { Test-CodeFile $_ } | Sort-Object -Unique)
}

function Get-AddedLines {
    param([string]$Path)

    # Untracked files have no diff, so treat their current content as added lines.
    $untracked = Invoke-Git @("ls-files", "--others", "--exclude-standard", "--", $Path)
    if ($untracked.Count -gt 0) {
        return @(Get-Content -Path $Path | ForEach-Object { "+$_" })
    }

    if ($Mode -eq "staged") {
        return @(Invoke-Git @("diff", "--cached", "--unified=0", "--", $Path) | Where-Object { $_.StartsWith("+") -and -not $_.StartsWith("+++") })
    }

    return @(Invoke-Git @("diff", "--unified=0", "--", $Path) | Where-Object { $_.StartsWith("+") -and -not $_.StartsWith("+++") })
}

function Test-CommentLine {
    param(
        [string]$Path,
        [string]$Line
    )

    # Match comment syntax by language, avoiding false positives such as C++ #include.
    $extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    $text = $Line.Substring(1).Trim()

    if ($extension -in @(".cpp", ".h", ".hpp", ".c", ".cc", ".cxx")) {
        return $text.StartsWith("//") -or $text.StartsWith("/*") -or $text.StartsWith("*") -or $text.Contains("//") -or $text.Contains("/*")
    }

    if ($extension -in @(".cmake", ".ps1") -or [System.IO.Path]::GetFileName($Path) -eq "CMakeLists.txt") {
        return $text.StartsWith("#")
    }

    if ($extension -in @(".bat", ".cmd")) {
        return $text.StartsWith("REM", [System.StringComparison]::OrdinalIgnoreCase) -or $text.StartsWith("::")
    }

    return $false
}

$failures = @()

foreach ($file in Get-ChangedCodeFiles) {
    $addedLines = @(Get-AddedLines $file)
    $nonBlankAdds = @($addedLines | Where-Object { $_.Substring(1).Trim().Length -gt 0 })
    $commentAdds = @($nonBlankAdds | Where-Object { Test-CommentLine $file $_ })

    # A file that adds implementation code must also add or update comments.
    if ($nonBlankAdds.Count -gt 0 -and $commentAdds.Count -eq 0) {
        $failures += $file
    }
}

if ($failures.Count -gt 0) {
    Write-Host "Comment policy check failed. Add or update comments in these changed code files:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}

Write-Host "Comment policy check passed." -ForegroundColor Green
