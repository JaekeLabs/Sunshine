Set Shell = CreateObject("WScript.Shell")
Shell.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""C:\Tools\Sunshine-Edge\SunshineEdgeTray.ps1""", 0, False
