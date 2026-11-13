# List references to PS2 ELF addresses with Ghidra headless (same project as decompile.ps1).
#   powershell -File tools/ghidra/xrefs.ps1 0x4da468 0x388d90
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Addrs)
$root = Split-Path (Split-Path $PSScriptRoot)
$env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.5.8-hotspot'
$ghidra = 'C:\Program Files (x86)\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat'
$proj = Join-Path $root 'extracted\ghidra'
& $ghidra $proj cars_ee -process SLUS_211.51 -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript XrefsTo.java @Addrs 2>&1 |
    Select-String -Pattern 'xref |ERROR|Exception'
