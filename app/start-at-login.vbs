' Cadence - start the app at login.
'
' Install by copying this into the Startup folder:
'   copy start-at-login.vbs "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\"
' Remove it from there to stop Cadence starting with Windows.
'
' Why a .vbs and not a .bat: WScript.Shell launches without a console, so
' nothing flashes on screen at login. A .bat would blink a terminal every time
' you sign in.
'
' Why pythonw.exe and not python.exe: pythonw has no console attached at all,
' so the process cannot be killed by a stray Ctrl-C in an unrelated window and
' does not hold a phantom terminal for the life of the session.
'
' Window style 1 rather than 0. The thing this replaces was a headless host and
' was deliberately hidden; this is an app, and an app you cannot see is an app
' you cannot tell is running -- which is exactly how the old host spent twenty
' minutes dead without anyone noticing.
'
' Cadence writes app\cadence.log, which is where to look when something is
' wrong. It retries a missing camera board forever rather than exiting, so
' starting before the board is powered is fine.

Option Explicit

Dim sh, py, script, here

here = Left(WScript.ScriptFullName, InStrRev(WScript.ScriptFullName, "\") - 1)
script = here & "\cadence.pyw"

' The interpreter that has mediapipe installed. Change this if you move Python;
' the plain "python" on PATH may be a different install without the packages.
py = "C:\Python314\pythonw.exe"

Set sh = CreateObject("WScript.Shell")

If Not CreateObject("Scripting.FileSystemObject").FileExists(py) Then
  sh.Popup "Cadence: interpreter not found at " & py, 10, "Cadence", 48
  WScript.Quit 1
End If

sh.CurrentDirectory = here
sh.Run """" & py & """ """ & script & """ --host cadence-cam.local", 1, False
