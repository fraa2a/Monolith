$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('monolith path test ' + [guid]::NewGuid())
New-Item -ItemType Directory $root | Out-Null
$oldPath = $env:PATH
$oldVcpkg = $env:VCPKG_ROOT
try {
    Copy-Item "$PSScriptRoot/../../build.bat" $root
    $vcpkg = Join-Path $root 'vcpkg with spaces'
    New-Item -ItemType Directory "$vcpkg/scripts/buildsystems" -Force | Out-Null
    New-Item -ItemType File "$vcpkg/scripts/buildsystems/vcpkg.cmake" | Out-Null
    $env:VCPKG_ROOT = $vcpkg
    $env:MONOLITH_BUILD_ARGS = Join-Path $root 'args.txt'
    Add-Type -TypeDefinition @'
using System;
using System.IO;
public class CmakeProbe {
    public static int Main(string[] args) {
        if (Array.IndexOf(args, "-S") >= 0) File.WriteAllLines(Environment.GetEnvironmentVariable("MONOLITH_BUILD_ARGS"), args);
        return 0;
    }
}
'@ -OutputAssembly "$root/cmake.exe" -OutputType ConsoleApplication
    $env:PATH = "$root;$oldPath"
    & cmd /c "call `"$root/build.bat`" < NUL"
    if ($LASTEXITCODE -ne 0) { throw 'build.bat failed' }
    $args = [IO.File]::ReadAllLines($env:MONOLITH_BUILD_ARGS)
    if ($args -notcontains "-DCMAKE_TOOLCHAIN_FILE=$vcpkg\scripts\buildsystems\vcpkg.cmake") { throw 'toolchain definition split at spaces' }
    $mt = @($args | Where-Object { $_.StartsWith('-DCMAKE_MT:FILEPATH=') })
    if ($mt.Count -ne 1 -or $mt[0] -notmatch 'Program Files') { throw 'SDK definition absent or split at spaces' }
} finally {
    $env:PATH = $oldPath
    $env:VCPKG_ROOT = $oldVcpkg
    Remove-Item Env:MONOLITH_BUILD_ARGS -ErrorAction SilentlyContinue
    Remove-Item $root -Recurse -Force
}
