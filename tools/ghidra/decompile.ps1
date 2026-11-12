# Decompile PS2 ELF functions with Ghidra headless.
#   powershell -File tools/ghidra/decompile.ps1 0x2b2a80 0x225810
# Output: extracted/ghidra/c/<entry>.c
# Needs the ghidra-emotionengine-reloaded extension (%APPDATA%\ghidra\ghidra_12.2_PUBLIC\Extensions)
# and the project made once by:
#   analyzeHeadless.bat extracted\ghidra cars_ee -import extracted\SLUS_211.51 -processor r5900:LE:32:default
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Addrs)
$root = Split-Path (Split-Path $PSScriptRoot)
$env:JAVA_HOME = 'C:\Program Files\Eclipse Adoptium\jdk-25.0.5.8-hotspot'
$ghidra = 'C:\Program Files (x86)\ghidra_12.2_PUBLIC\support\analyzeHeadless.bat'
$proj = Join-Path $root 'extracted\ghidra'
$out = Join-Path $proj 'c'
& $ghidra $proj cars_ee -process SLUS_211.51 -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript DecompileAt.java $out @Addrs 2>&1 |
    Select-String -Pattern 'wrote|no function|ERROR|Exception'
