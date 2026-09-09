[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDir,

    [Parameter(Mandatory = $true)]
    [string]$VcpkgBinDir
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $RuntimeDir -PathType Container)) {
    throw "Runtime directory does not exist: $RuntimeDir"
}
if (-not (Test-Path -LiteralPath $VcpkgBinDir -PathType Container)) {
    throw "vcpkg runtime directory does not exist: $VcpkgBinDir"
}

# Scope: EXE/DLL files directly in RuntimeDir and their transitive imports.
# Sidecars in subdirectories (such as ui/) and LoadLibrary-only dependencies
# are not validated here. This does not replace a clean-Windows smoke test.
$dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
$windowsSystem = Join-Path $env:WINDIR "System32"
$runtimeFiles = @(Get-ChildItem -LiteralPath $RuntimeDir -File | Where-Object {
    $_.Extension -ieq ".exe" -or $_.Extension -ieq ".dll"
})
if ($runtimeFiles.Count -eq 0) {
    throw "No native runtime binaries found in: $RuntimeDir"
}

$runtimeByName = @{}
$pending = [System.Collections.Generic.Queue[string]]::new()
foreach ($file in $runtimeFiles) {
    $runtimeByName[$file.Name.ToLowerInvariant()] = $file.FullName
    $pending.Enqueue($file.FullName)
}

$visited = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
$missing = [System.Collections.Generic.List[string]]::new()

while ($pending.Count -ne 0) {
    $binary = $pending.Dequeue()
    if (-not $visited.Add($binary)) {
        continue
    }

    $imports = & $dumpbin /NOLOGO /DEPENDENTS $binary 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $binary"
    }

    foreach ($line in $imports) {
        $match = [regex]::Match([string]$line, '^\s+([^\s\\/]+\.dll)\s*$', 'IgnoreCase')
        if (-not $match.Success) {
            continue
        }

        $dependency = $match.Groups[1].Value
        $key = $dependency.ToLowerInvariant()
        if ($runtimeByName.ContainsKey($key)) {
            $pending.Enqueue($runtimeByName[$key])
            continue
        }

        # Prefer the vcpkg copy over anything coincidentally installed on the
        # CI host. API-set DLLs and Windows-owned DLLs remain OS dependencies.
        if (Test-Path -LiteralPath (Join-Path $VcpkgBinDir $dependency) -PathType Leaf) {
            $missing.Add("$dependency (imported by $([IO.Path]::GetFileName($binary)))")
            continue
        }
        if ($dependency.StartsWith("api-ms-win-", [StringComparison]::OrdinalIgnoreCase) -or
            $dependency.StartsWith("ext-ms-win-", [StringComparison]::OrdinalIgnoreCase) -or
            (Test-Path -LiteralPath (Join-Path $windowsSystem $dependency) -PathType Leaf)) {
            continue
        }

        $missing.Add("$dependency (imported by $([IO.Path]::GetFileName($binary)); unresolved)")
    }
}

if ($missing.Count -ne 0) {
    $details = ($missing | Sort-Object -Unique) -join ", "
    throw "Native runtime dependency closure is incomplete: $details"
}

Write-Host "Verified static import closure for $($visited.Count) root-level native binary/binaries in $RuntimeDir (subdirectories excluded)"
(Get-ChildItem -LiteralPath $RuntimeDir -Filter "*.dll" -File).Name |
    Sort-Object |
    ForEach-Object { Write-Host "  $_" }
