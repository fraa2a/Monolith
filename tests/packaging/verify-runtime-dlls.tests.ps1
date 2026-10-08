$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
$previousWindows = $env:WINDIR
$verifier = Join-Path $PSScriptRoot '../../scripts/verify-runtime-dlls.ps1'
$global:MonolithTestImports = @{}

function global:dumpbin.exe {
    param($NoLogo, $Dependents, $Binary)
    $global:LASTEXITCODE = 0
    foreach ($dependency in $global:MonolithTestImports[[IO.Path]::GetFileName($Binary)]) {
        "    $dependency"
    }
}

function Assert-Closure([bool]$ShouldPass, [string]$Label) {
    $passed = $false
    try {
        & $verifier -RuntimeDir "$root/runtime" -VcpkgBinDir "$root/vcpkg" | Out-Null
        $passed = $true
    } catch {
        if ($ShouldPass) { throw }
        if ($_.Exception.Message -notlike '*dependency closure is incomplete*') { throw }
    }
    if ($passed -ne $ShouldPass) { throw "Unexpected result: $Label" }
    Write-Host "PASS: $Label"
}

try {
    foreach ($directory in @('runtime/ui', 'vcpkg', 'windows/System32')) {
        New-Item -ItemType Directory -Path "$root/$directory" -Force | Out-Null
    }
    $env:WINDIR = "$root/windows"
    Set-Content "$root/runtime/Monolith.exe" ''
    Set-Content "$root/windows/System32/kernel32.dll" ''
    $global:MonolithTestImports = @{'Monolith.exe' = @('kernel32.dll', 'api-ms-win-core-file-l1-1-0.dll')}
    Assert-Closure $true 'Windows and API-set imports'

    Set-Content "$root/vcpkg/aom.dll" ''
    $global:MonolithTestImports['Monolith.exe'] = @('aom.dll')
    Assert-Closure $false 'Missing vcpkg import'
    Set-Content "$root/runtime/aom.dll" ''
    $global:MonolithTestImports['aom.dll'] = @('kernel32.dll')
    Assert-Closure $true 'Complete transitive import closure'

    Set-Content "$root/windows/System32/vcruntime140.dll" ''
    $global:MonolithTestImports['aom.dll'] = @('vcruntime140.dll')
    Assert-Closure $false 'VC runtime installed on CI is not a packaged runtime'
    Set-Content "$root/runtime/vcruntime140.dll" ''
    Assert-Closure $true 'App-local VC runtime'

    Set-Content "$root/runtime/ui/Monolith.UI.exe" ''
    $global:MonolithTestImports['Monolith.UI.exe'] = @('vcruntime140.dll')
    Assert-Closure $false 'Root DLL does not satisfy a sidecar import'
    Set-Content "$root/runtime/ui/vcruntime140.dll" ''
    Assert-Closure $true 'Sidecar has its own import closure'

    $global:MonolithTestImports['Monolith.UI.exe'] = @('unknown.dll')
    Assert-Closure $false 'Unknown dependency is rejected'
} finally {
    $env:WINDIR = $previousWindows
    Remove-Variable MonolithTestImports -Scope Global
    Remove-Item Function:/dumpbin.exe
    Remove-Item -LiteralPath $root -Recurse -Force
}
