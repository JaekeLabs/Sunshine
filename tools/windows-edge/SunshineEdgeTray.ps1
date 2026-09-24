Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Prevent multiple tray-controller instances.
$CreatedNew = $false
$TrayMutex = New-Object System.Threading.Mutex(
    $true,
    'Local\SunshineEdgeTrayController',
    [ref]$CreatedNew
)

if (-not $CreatedNew) {
    exit
}

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class NativeIcon
{
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern bool DestroyIcon(IntPtr handle);
}
"@

$RendererSource = @"
using System.Drawing;
using System.Windows.Forms;

public class DarkMenuRenderer : ToolStripRenderer
{
    private readonly Color Background = Color.FromArgb(32, 32, 32);
    private readonly Color Selected   = Color.FromArgb(55, 55, 55);
    private readonly Color Text       = Color.FromArgb(235, 235, 235);
    private readonly Color Disabled   = Color.FromArgb(125, 125, 125);
    private readonly Color Border     = Color.FromArgb(70, 70, 70);

    // Public property avoids the harmless Add-Type warning about a type
    // defining no public methods or properties.
    public Color SelectionColor
    {
        get { return Selected; }
    }

    protected override void OnRenderToolStripBackground(ToolStripRenderEventArgs e)
    {
        using (SolidBrush brush = new SolidBrush(Background))
        {
            e.Graphics.FillRectangle(
                brush,
                new Rectangle(Point.Empty, e.ToolStrip.Size)
            );
        }
    }

    protected override void OnRenderMenuItemBackground(ToolStripItemRenderEventArgs e)
    {
        Rectangle rect = new Rectangle(Point.Empty, e.Item.Size);

        using (SolidBrush brush = new SolidBrush(
            e.Item.Selected ? Selected : Background))
        {
            e.Graphics.FillRectangle(brush, rect);
        }
    }

    protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
    {
        e.TextColor = e.Item.Enabled ? Text : Disabled;
        base.OnRenderItemText(e);
    }

    protected override void OnRenderSeparator(ToolStripSeparatorRenderEventArgs e)
    {
        using (Pen pen = new Pen(Border))
        {
            int y = e.Item.Height / 2;
            e.Graphics.DrawLine(pen, 4, y, e.Item.Width - 4, y);
        }
    }

    protected override void OnRenderToolStripBorder(ToolStripRenderEventArgs e)
    {
        using (Pen pen = new Pen(Border))
        {
            Rectangle rect = new Rectangle(
                0,
                0,
                e.ToolStrip.Width - 1,
                e.ToolStrip.Height - 1
            );

            e.Graphics.DrawRectangle(pen, rect);
        }
    }
}
"@

$WinFormsAssembly = [System.Windows.Forms.Form].Assembly.Location
$DrawingAssembly  = [System.Drawing.Color].Assembly.Location

Add-Type -TypeDefinition $RendererSource `
    -ReferencedAssemblies $WinFormsAssembly,$DrawingAssembly

$SunshineDir  = 'C:\Tools\Sunshine-Edge'
$SunshineExe  = Join-Path $SunshineDir 'sunshine.exe'
$SunshineConf = Join-Path $SunshineDir 'config\sunshine.conf'
$WebUI        = 'https://localhost:47990'

$StartupDir      = [Environment]::GetFolderPath('Startup')
$StartupShortcut = Join-Path $StartupDir 'Sunshine Edge Tray.lnk'
$SilentLauncher  = Join-Path $SunshineDir 'SunshineEdgeTray.vbs'

function Get-SunshineEdgeProcess {
    Get-Process -Name sunshine -ErrorAction SilentlyContinue |
        Where-Object {
            try {
                [string]::Equals(
                    $_.Path,
                    $SunshineExe,
                    [System.StringComparison]::OrdinalIgnoreCase
                )
            }
            catch {
                $false
            }
        } |
        Select-Object -First 1
}

function Get-EdgeWindow {
    Get-Process msedge -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
}

function Get-EdgeExecutable {
    $Candidates = @(
        (Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft\Edge\Application\msedge.exe'),
        (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Microsoft\Edge\Application\msedge.exe')
    )

    foreach ($Candidate in $Candidates) {
        if ($Candidate -and (Test-Path $Candidate)) {
            return $Candidate
        }
    }

    return 'msedge.exe'
}

function Ensure-EdgeWindow {
    if (Get-EdgeWindow) {
        return $true
    }

    Start-Process (Get-EdgeExecutable)

    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250

        if (Get-EdgeWindow) {
            return $true
        }
    }

    return $false
}

function Get-StreamAudioEnabled {
    if (-not (Test-Path $SunshineConf)) {
        return $false
    }

    $Match = Select-String -Path $SunshineConf -Pattern '^\s*stream_audio\s*=\s*(.+?)\s*$' |
        Select-Object -Last 1

    if (-not $Match) {
        return $false
    }

    $Value = $Match.Matches[0].Groups[1].Value.Trim().ToLowerInvariant()

    return $Value -in @(
        'enabled',
        'enable',
        'true',
        'yes',
        'on',
        '1'
    )
}

function Set-StreamAudioEnabled {
    param(
        [bool]$Enabled
    )

    $Value = if ($Enabled) { 'enabled' } else { 'disabled' }

    $Lines = [System.Collections.Generic.List[string]]::new()

    foreach ($Line in [System.IO.File]::ReadAllLines($SunshineConf)) {
        $Lines.Add($Line)
    }

    $Found = $false

    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match '^\s*stream_audio\s*=') {
            $Lines[$i] = "stream_audio = $Value"
            $Found = $true
        }
    }

    if (-not $Found) {
        $Lines.Add("stream_audio = $Value")
    }

    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

    [System.IO.File]::WriteAllLines(
        $SunshineConf,
        $Lines,
        $Utf8NoBom
    )
}

function Get-AutostartEnabled {
    return Test-Path $StartupShortcut
}

function Set-AutostartEnabled {
    param(
        [bool]$Enabled
    )

    if ($Enabled) {
        $Shell = New-Object -ComObject WScript.Shell
        $Shortcut = $Shell.CreateShortcut($StartupShortcut)

        $Shortcut.TargetPath = "$env:SystemRoot\System32\wscript.exe"
        $Shortcut.Arguments = "`"$SilentLauncher`""
        $Shortcut.WorkingDirectory = $SunshineDir
        $Shortcut.IconLocation = "$SunshineExe,0"
        $Shortcut.Save()
    }
    else {
        Remove-Item $StartupShortcut -Force -ErrorAction SilentlyContinue
    }
}

function New-TintedIcon {
    param(
        [System.Drawing.Icon]$SourceIcon,

        [ValidateSet('Gray', 'Orange')]
        [string]$Mode
    )

    $Source = $SourceIcon.ToBitmap()
    $Bitmap = New-Object System.Drawing.Bitmap 32, 32

    $Graphics = [System.Drawing.Graphics]::FromImage($Bitmap)
    $Graphics.DrawImage($Source, 0, 0, 32, 32)
    $Graphics.Dispose()
    $Source.Dispose()

    for ($x = 0; $x -lt $Bitmap.Width; $x++) {
        for ($y = 0; $y -lt $Bitmap.Height; $y++) {
            $Pixel = $Bitmap.GetPixel($x, $y)

            if ($Pixel.A -eq 0) {
                continue
            }

            $Brightness = ($Pixel.R + $Pixel.G + $Pixel.B) / (3.0 * 255.0)
            $Brightness = [Math]::Max(0.45, $Brightness)

            if ($Mode -eq 'Gray') {
                $Value = [int](175 * $Brightness)

                $Color = [System.Drawing.Color]::FromArgb(
                    $Pixel.A,
                    $Value,
                    $Value,
                    $Value
                )
            }
            else {
                $R = [int](255 * $Brightness)
                $G = [int](170 * $Brightness)
                $B = [int](35 * $Brightness)

                $Color = [System.Drawing.Color]::FromArgb(
                    $Pixel.A,
                    [Math]::Min(255, $R),
                    [Math]::Min(255, $G),
                    [Math]::Min(255, $B)
                )
            }

            $Bitmap.SetPixel($x, $y, $Color)
        }
    }

    $Handle = $Bitmap.GetHicon()
    $Icon = [System.Drawing.Icon]::FromHandle($Handle).Clone()

    [NativeIcon]::DestroyIcon($Handle) | Out-Null
    $Bitmap.Dispose()

    return $Icon
}

$BaseIcon = [System.Drawing.Icon]::ExtractAssociatedIcon($SunshineExe)
$IconOff  = New-TintedIcon -SourceIcon $BaseIcon -Mode Gray
$IconOn   = New-TintedIcon -SourceIcon $BaseIcon -Mode Orange
$BaseIcon.Dispose()

$Notify = New-Object System.Windows.Forms.NotifyIcon
$Notify.Visible = $true

$Menu = New-Object System.Windows.Forms.ContextMenuStrip
$Menu.ShowImageMargin = $false
$Menu.ShowCheckMargin = $false
$Menu.BackColor = [System.Drawing.Color]::FromArgb(32, 32, 32)
$Menu.ForeColor = [System.Drawing.Color]::FromArgb(235, 235, 235)
$Menu.Renderer = New-Object DarkMenuRenderer

$StartItem = $Menu.Items.Add('Start Sunshine Edge')
$StopItem  = $Menu.Items.Add('Stop Sunshine Edge')

$Menu.Items.Add(
    (New-Object System.Windows.Forms.ToolStripSeparator)
) | Out-Null

$AudioItem = New-Object System.Windows.Forms.ToolStripMenuItem
$AudioItem.Text = 'Stream Audio'
$Menu.Items.Add($AudioItem) | Out-Null

$AutoStartItem = New-Object System.Windows.Forms.ToolStripMenuItem
$AutoStartItem.Text = 'Start with Windows'
$Menu.Items.Add($AutoStartItem) | Out-Null

$Menu.Items.Add(
    (New-Object System.Windows.Forms.ToolStripSeparator)
) | Out-Null

$WebItem  = $Menu.Items.Add('Open Sunshine Web UI')
$EdgeItem = $Menu.Items.Add('Open Edge')

$Menu.Items.Add(
    (New-Object System.Windows.Forms.ToolStripSeparator)
) | Out-Null

$ExitItem = $Menu.Items.Add('Exit Tray Controller')

foreach ($Item in $Menu.Items) {
    $Item.BackColor = [System.Drawing.Color]::FromArgb(32, 32, 32)
    $Item.ForeColor = [System.Drawing.Color]::FromArgb(235, 235, 235)
}

$Notify.ContextMenuStrip = $Menu

function Update-TrayState {
    $Running = $null -ne (Get-SunshineEdgeProcess)

    if ($Running) {
        $Notify.Icon = $IconOn
        $Notify.Text = 'Sunshine Edge - Running'

        $StartItem.Enabled = $false
        $StopItem.Enabled  = $true
    }
    else {
        $Notify.Icon = $IconOff
        $Notify.Text = 'Sunshine Edge - Off'

        $StartItem.Enabled = $true
        $StopItem.Enabled  = $false
    }

    if (Get-StreamAudioEnabled) {
        $AudioItem.Text = '✓  Stream Audio'
    }
    else {
        $AudioItem.Text = 'Stream Audio'
    }

    if (Get-AutostartEnabled) {
        $AutoStartItem.Text = '✓  Start with Windows'
    }
    else {
        $AutoStartItem.Text = 'Start with Windows'
    }
}

function Start-SunshineEdge {
    if (Get-SunshineEdgeProcess) {
        Update-TrayState
        return
    }

    if (-not (Ensure-EdgeWindow)) {
        $Notify.BalloonTipTitle = 'Sunshine Edge'
        $Notify.BalloonTipText = 'Could not find or open a visible Microsoft Edge window.'
        $Notify.ShowBalloonTip(4000)
        return
    }

    Start-Process -FilePath $SunshineExe -WorkingDirectory $SunshineDir -WindowStyle Hidden

    Start-Sleep -Milliseconds 750
    Update-TrayState
}

function Stop-SunshineEdge {
    $Process = Get-SunshineEdgeProcess

    if ($Process) {
        Stop-Process -Id $Process.Id -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }

    Update-TrayState
}

$StartItem.add_Click({
    Start-SunshineEdge
})

$StopItem.add_Click({
    Stop-SunshineEdge
})

$AudioItem.add_Click({
    $NewState = -not (Get-StreamAudioEnabled)

    Set-StreamAudioEnabled -Enabled $NewState

    $WasRunning = $null -ne (Get-SunshineEdgeProcess)

    if ($WasRunning) {
        Stop-SunshineEdge
        Start-SunshineEdge

        $Notify.BalloonTipTitle = 'Sunshine Edge'

        if ($NewState) {
            $Notify.BalloonTipText = 'Audio streaming enabled. Sunshine restarted.'
        }
        else {
            $Notify.BalloonTipText = 'Audio streaming disabled. Sunshine restarted.'
        }

        $Notify.ShowBalloonTip(2500)
    }

    Update-TrayState
})

$AutoStartItem.add_Click({
    $NewState = -not (Get-AutostartEnabled)

    Set-AutostartEnabled -Enabled $NewState
    Update-TrayState
})

$WebItem.add_Click({
    Start-Process $WebUI
})

$EdgeItem.add_Click({
    Start-Process (Get-EdgeExecutable)
})

$Notify.add_MouseDoubleClick({
    param($Sender, $EventArgs)

    if ($EventArgs.Button -eq [System.Windows.Forms.MouseButtons]::Left) {
        if (Get-SunshineEdgeProcess) {
            Stop-SunshineEdge
        }
        else {
            Start-SunshineEdge
        }
    }
})

$Menu.add_Opening({
    Update-TrayState
})

$ExitItem.add_Click({
    $Timer.Stop()

    # Exit means the custom Sunshine instance is stopped as well.
    $Process = Get-SunshineEdgeProcess

    if ($Process) {
        Stop-Process -Id $Process.Id -ErrorAction SilentlyContinue
    }

    $Notify.Visible = $false
    $Notify.Dispose()
    $Menu.Dispose()
    $IconOff.Dispose()
    $IconOn.Dispose()

    $TrayMutex.ReleaseMutex()
    $TrayMutex.Dispose()

    [System.Windows.Forms.Application]::Exit()
})

$Timer = New-Object System.Windows.Forms.Timer
$Timer.Interval = 1000

$Timer.add_Tick({
    Update-TrayState
})

$Timer.Start()

Update-TrayState

[System.Windows.Forms.Application]::Run()
