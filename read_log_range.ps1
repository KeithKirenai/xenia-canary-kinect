$fileStream = [System.IO.File]::Open("build\bin\Windows\Release\xenia.log", [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
$reader = New-Object System.IO.StreamReader($fileStream)
$n = 0
while (($line = $reader.ReadLine()) -ne $null) {
    $n++
    if ($n -ge 4790 -and $n -le 4820) {
        Write-Host ("{0}: {1}" -f $n, $line)
    }
}
$reader.Close()
$fileStream.Close()
