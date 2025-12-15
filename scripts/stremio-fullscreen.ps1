$stremioPath = "C:\Users\$env:UserName\AppData\Local\Programs\Stremio\stremio-shell-ng.exe"

Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;

public class JobObject {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string lpName);

    [DllImport("kernel32.dll")]
    public static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll")]
    public static extern bool SetInformationJobObject(IntPtr hJob, int JobObjectInfoClass, IntPtr lpJobObjectInfo, uint cbJobObjectInfoLength);

    public static IntPtr CreateKillOnCloseJob() {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        IntPtr extendedInfo = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)));
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = 0x2000;
        Marshal.StructureToPtr(info, extendedInfo, false);
        SetInformationJobObject(job, 9, extendedInfo, (uint)Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)));
        Marshal.FreeHGlobal(extendedInfo);
        return job;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IO_COUNTERS {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }
}

public class Mouse {
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);
}

public class Keyboard {
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
}

public class Window {
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
}
"@

Write-Host "Starting Stremio..."
$job = [JobObject]::CreateKillOnCloseJob()

$stremioProcess = Start-Process -FilePath $stremioPath -PassThru
[JobObject]::AssignProcessToJobObject($job, $stremioProcess.Handle)

# Poll for window to be ready
Write-Host "Waiting for window..."
$proc = $null
for ($i = 0; $i -lt 50; $i++) {
    $proc = Get-Process stremio-shell-ng -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
    if ($proc) { break }
    Start-Sleep -Milliseconds 100
}

# Attach runtime to job
$runtime = Get-Process -Name "stremio-runtime" -ErrorAction SilentlyContinue
if ($runtime) {
    [JobObject]::AssignProcessToJobObject($runtime, $runtime.Handle)
    Write-Host "Runtime attached to job"
}

# Wait for window to fully initialize
Start-Sleep -Seconds 1

if ($proc) {
    $hwnd = $proc.MainWindowHandle
    Write-Host "Window ready (HWND: $hwnd)"
    
    # Get secondary (non-primary) screen
    $secondaryScreen = [System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary } | Select-Object -First 1
    
    if ($secondaryScreen) {
        Write-Host "Moving to secondary screen: $($secondaryScreen.DeviceName)"
        $bounds = $secondaryScreen.Bounds
        
        # Move and maximize on secondary screen
        [Window]::SetWindowPos($hwnd, [IntPtr]::Zero, $bounds.X, $bounds.Y, $bounds.Width, $bounds.Height, 0x0040)
        Write-Host "Window moved to: $($bounds.X), $($bounds.Y) - Size: $($bounds.Width)x$($bounds.Height)"
    } else {
        Write-Host "No secondary screen found, using primary"
        $secondaryScreen = [System.Windows.Forms.Screen]::PrimaryScreen
    }
    
    # Global hardware keypress - F11
    [Keyboard]::keybd_event(0x7A, 0x57, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 50
    [Keyboard]::keybd_event(0x7A, 0x57, 2, [UIntPtr]::Zero)
    Write-Host "F11 sent"
    
    # Center mouse on secondary screen
    $centerX = $secondaryScreen.Bounds.X + ($secondaryScreen.Bounds.Width / 2)
    $centerY = $secondaryScreen.Bounds.Y + ($secondaryScreen.Bounds.Height / 2)
    [Mouse]::SetCursorPos($centerX, $centerY)
    Write-Host "Mouse centered at: $centerX, $centerY"
} else {
    Write-Host "Timeout waiting for window"
}

Write-Host "Monitoring process..."

# Poll until window closes
while ($true) {
    $stremioProc = Get-Process -Name "stremio-shell-ng" -ErrorAction SilentlyContinue
    if (-not $stremioProc -or $stremioProc.MainWindowHandle -eq 0) {
        Get-Process -Name "stremio*" -ErrorAction SilentlyContinue | Stop-Process -Force
        Write-Host "Stremio closed, exiting"
        break
    }
    Start-Sleep -Seconds 1
}