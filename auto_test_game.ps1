param (
    [int]$RuntimeSeconds = 30
)

$isoPath = "C:\Users\Carlos\Downloads\Kinect Adventures! (USA) (En,Ja,Fr,De,Es,It,Nl,Pt,Zh,Ko,Pl,Ru) (Rev 2).iso"
$xeniaExe = "build\bin\Windows\Release\xenia_canary.exe"

# Asegurar que no haya instancias previas corriendo
Stop-Process -Name "xenia_canary" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "Lanzando Xenia Canary con Kinect Adventures..."
$proc = Start-Process -FilePath $xeniaExe -ArgumentList "`"$isoPath`"" -PassThru

Write-Host "Ejecutando juego por $RuntimeSeconds segundos..."
Start-Sleep -Seconds $RuntimeSeconds

Write-Host "Cerrando emulador..."
if (!$proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2

Write-Host "`n--- RESULTADOS RELEVANTES DE NUI Y KINECT EN EL LOG ---"
$logPath = "build\bin\Windows\Release\xenia.log"
if (Test-Path $logPath) {
    $fs = [System.IO.File]::Open($logPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $r = New-Object System.IO.StreamReader($fs)
    $lines = New-Object System.Collections.Generic.List[string]
    while (($l = $r.ReadLine()) -ne $null) {
        if ($l -match "Xam|Nui|Kinect|Skeleton|0x2B|XamVoice|Error|crash|fatal") {
            $lines.Add($l)
            if ($lines.Count -gt 80) {
                $lines.RemoveAt(0)
            }
        }
    }
    $r.Close()
    $fs.Close()
    $lines | ForEach-Object { Write-Host $_ }
} else {
    Write-Host "No se encontro el archivo xenia.log"
}
