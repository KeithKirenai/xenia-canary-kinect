$csharp = @"
using System;
using System.Runtime.InteropServices;

public class TestKinectLed {
    [DllImport("Kinect10.dll")]
    public static extern int NuiInitialize(uint flags);

    [DllImport("Kinect10.dll")]
    public static extern int NuiSkeletonTrackingEnable(IntPtr hNextFrameEvent, uint dwFlags);

    [DllImport("Kinect10.dll")]
    public static extern int NuiImageStreamOpen(int imageType, int resolution, uint flags, uint frameLimit, IntPtr nextFrameEvent, out IntPtr streamHandle);

    [DllImport("Kinect10.dll")]
    public static extern void NuiShutdown();
}
"@

Add-Type -TypeDefinition $csharp

Write-Host "Inicializando Kinect con COLOR + DEPTH_AND_PLAYER_INDEX + SKELETON..."
# 0x01 (depth+player) | 0x02 (color) | 0x08 (skeleton) = 0x0B
$hr = [TestKinectLed]::NuiInitialize(0x0B)
Write-Host ("NuiInitialize result: 0x{0:X8}" -f $hr)

$hStream = [IntPtr]::Zero
$hrStream = [TestKinectLed]::NuiImageStreamOpen(1, 2, 0, 2, [IntPtr]::Zero, [ref]$hStream)
Write-Host ("NuiImageStreamOpen (Color) result: 0x{0:X8}, handle=0x{1:X}" -f $hrStream, $hStream.ToInt64())

$hrTrack = [TestKinectLed]::NuiSkeletonTrackingEnable([IntPtr]::Zero, 0)
Write-Host ("NuiSkeletonTrackingEnable result: 0x{0:X8}" -f $hrTrack)

Write-Host "Manteniendo streams activos por 10 segundos para observar el LED verde..."
Start-Sleep -Seconds 10

[TestKinectLed]::NuiShutdown()
Write-Host "Apagado y liberado."
