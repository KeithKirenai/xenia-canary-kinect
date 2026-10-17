$fs = [System.IO.File]::Open("build\bin\Windows\Release\xenia.log", [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
$r = New-Object System.IO.StreamReader($fs)
$lines = New-Object System.Collections.Generic.List[string]
while (($l = $r.ReadLine()) -ne $null) {
    if ($l -notmatch "KeWait|NtWait|KeDelay|XamVoiceSubmit|XamVoiceHeadset|XamInput|XamTask|handle\[|00021012") {
        $lines.Add($l)
        if ($lines.Count -gt 60) {
            $lines.RemoveAt(0)
        }
    }
}
$r.Close()
$fs.Close()
$lines
