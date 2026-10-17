param(
    [string]$ImagePath = "test_img.png"
)

Add-Type -AssemblyName System.Drawing
[Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime] | Out-Null
[Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime] | Out-Null
[Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime] | Out-Null

function Get-OcrText([string]$filePath) {
    $fullPath = (Resolve-Path $filePath).Path
    $fileTask = [Windows.Storage.StorageFile]::GetFileFromPathAsync($fullPath)
    $asTaskMethod = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { 
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -like 'IAsyncOperation*'
    } | Select-Object -First 1

    $file = $fileTask.GetAwaiter().GetResult()
    $stream = $file.OpenAsync([Windows.Storage.FileAccessMode]::Read).GetAwaiter().GetResult()
    $decoder = [Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream).GetAwaiter().GetResult()
    $bitmap = $decoder.GetSoftwareBitmapAsync().GetAwaiter().GetResult()
    
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    $ocrResult = $engine.RecognizeAsync($bitmap).GetAwaiter().GetResult()
    return $ocrResult.Text
}

if (Test-Path $ImagePath) {
    $text = Get-OcrText $ImagePath
    Write-Output "OCR Result: $text"
} else {
    Write-Output "File not found: $ImagePath"
}
