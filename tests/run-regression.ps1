# Compile the production parsers and mailbox code into an isolated UEFI test app.
[CmdletBinding()]
param([switch]$BuildOnly, [string]$ApplicationPath, [string]$IniPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root '.qemu-review-tests'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$instances = & $vswhere -all -products * -prerelease -format json | ConvertFrom-Json
$msbuild = $null
foreach ($instance in ($instances | Sort-Object { [version]$_.installationVersion } -Descending)) {
    $compiler = Get-ChildItem -LiteralPath (Join-Path $instance.installationPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } -Descending |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\Hostx64\x64\ml64.exe') } | Select-Object -First 1
    $candidate = Join-Path $instance.installationPath 'MSBuild\Current\Bin\MSBuild.exe'
    if ($compiler -and (Test-Path $candidate)) { $msbuild = $candidate; break }
}
if (-not $msbuild) { throw 'No installed MSBuild with an x64 assembler was found.' }
[xml]$project = Get-Content -LiteralPath (Join-Path $root 'src\UnderVolter.vcxproj') -Raw
$ns = [Xml.XmlNamespaceManager]::new($project.NameTable)
$ns.AddNamespace('p', $project.DocumentElement.NamespaceURI)
$included = @('UnderVolter.c', 'Config.c', 'SecureBootEnroll.c', 'NvramSetup.c', 'CpuMailboxes.c')
foreach ($item in @($project.SelectNodes('//p:ItemGroup/*[@Include]', $ns))) {
    if ($item.LocalName -notin @('ClCompile', 'ClInclude', 'MASM')) { continue }
    $relative = $item.GetAttribute('Include')
    if ($included -contains $relative) { $null = $item.ParentNode.RemoveChild($item); continue }
    $item.SetAttribute('Include', (Join-Path (Join-Path $root 'src') $relative))
}
$group = $project.CreateElement('ItemGroup', $project.DocumentElement.NamespaceURI)
$source = $project.CreateElement('ClCompile', $project.DocumentElement.NamespaceURI)
$source.SetAttribute('Include', (Join-Path $PSScriptRoot 'Regression.c'))
$null = $group.AppendChild($source); $null = $project.DocumentElement.AppendChild($group)
$projectPath = Join-Path $work 'Regression.vcxproj'
$project.Save($projectPath)
& $msbuild $projectPath /t:Build /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$root\" "/p:OutDir=$work\bin\" "/p:IntDir=$work\obj\" /p:TargetName=UnderVolter /v:minimal /nologo
if ($LASTEXITCODE) { throw "Regression build failed: $LASTEXITCODE" }
if (-not $BuildOnly) {
    if ($ApplicationPath) {
        New-Item -ItemType Directory -Force -Path (Join-Path $work 'vm\esp') | Out-Null
        Copy-Item -LiteralPath $ApplicationPath -Destination (Join-Path $work 'vm\esp\application.efi') -Force
    } else {
        Remove-Item -LiteralPath (Join-Path $work 'vm\esp\application.efi') -ErrorAction SilentlyContinue
    }
    if ($IniPath) { Copy-Item -LiteralPath $IniPath -Destination (Join-Path $work 'bin\UnderVolter.ini') -Force }
    & (Join-Path $root 'run-qemu.ps1') -Headless -NoReboot -BinaryDir (Join-Path $work 'bin') -VmDir (Join-Path $work 'vm') -Cpu CoffeeLake -Cores 4
    if ($LASTEXITCODE) { throw 'QEMU failed.' }
    $log = Get-Content -LiteralPath (Join-Path $work 'vm\logs\uefi-serial.log') -Raw
    if ($log -notmatch 'RESULT: (\d+) checks, 0 failures') { throw 'Regression tests did not report success. Inspect the serial log.' }
    Write-Host $Matches[0]
}
