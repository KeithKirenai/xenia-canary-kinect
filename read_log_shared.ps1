$fileStream = [System.IO.File]::Open("build\bin\Windows\Release\xenia.log", [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
$reader = New-Object System.IO.StreamReader($fileStream)
$n = 0
while (($line = $reader.ReadLine()) -ne $null) {
    $n++
    if ($line.Contains("NuiInitialize OK")) {
        Write-Host ("Found NuiInitialize OK at line {0}: {1}" -f $n, $line)
        break
    }
}
$reader.Close()
$fileStream.Close()
