$csharp = @"
using System;
using System.Runtime.InteropServices;

public class TestKinect {
    [DllImport("Kinect10.dll")]
    public static extern int NuiStatus();

    [DllImport("Kinect10.dll")]
    public static extern int NuiInitialize(uint flags);

    [DllImport("Kinect10.dll")]
    public static extern void NuiShutdown();
}
"@

Add-Type -TypeDefinition $csharp
$status = [TestKinect]::NuiStatus()
Write-Host ("Initial NuiStatus: 0x{0:X8}" -f $status)

$init = [TestKinect]::NuiInitialize(0x08)
Write-Host ("NuiInitialize result: 0x{0:X8}" -f $init)

Start-Sleep -Seconds 3
$status2 = [TestKinect]::NuiStatus()
Write-Host ("Status after 3s: 0x{0:X8}" -f $status2)

[TestKinect]::NuiShutdown()
Write-Host "Shutdown called."
