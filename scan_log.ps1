$lines = [System.IO.File]::ReadLines("build\bin\Windows\Release\xenia.log")
$lineNum = 0
foreach ($line in $lines) {
    $lineNum++
    if ($line -match "handle=F80000DC|handle:F80000DC") {
        Write-Host "${lineNum}: ${line}"
    }
}
