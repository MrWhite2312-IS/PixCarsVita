# Disassemble PS2 ELF functions with Ghidra headless (same project as decompile.ps1).
#   powershell -File tools/ghidra/disasm.ps1 0x22d820
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Addrs)
$root = Split-Path (Split-Path $PSScriptRoot)
$env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.5.8-hotspot'
$ghidra = 'C:\Program Files (x86)\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat'
$proj = Join-Path $root 'extracted\ghidra'
& $ghidra $proj cars_ee -process SLUS_211.51 -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript DisasmAt.java @Addrs 2>&1 |
    Select-String -Pattern 'asm |ERROR|Exception' | ForEach-Object { ($_.Line -replace '^.*DisasmAt.java> ', '') -replace ' \(GhidraScript\)\s*$', '' }
