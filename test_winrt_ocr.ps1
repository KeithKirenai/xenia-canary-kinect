[Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime] | Out-Null
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if ($null -ne $engine) {
    Write-Output "WINRT_OCR_READY"
} else {
    Write-Output "WINRT_OCR_NOT_FOUND"
}
