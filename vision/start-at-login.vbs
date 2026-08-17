' Cadence - start the headless vision host at login.
'
' Install by copying this into the Startup folder:
'   copy start-at-login.vbs "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\"
' Remove it from there to stop the host starting with Windows.
'
' Why a .vbs and not a .bat: WScript.Shell can launch with window style 0, so
' there is no console window and nothing flashes on screen at login. A .bat
' would blink a terminal every time you sign in.
'
' Why pythonw.exe and not python.exe: pythonw has no console attached at all,
' so the process cannot be killed by a stray Ctrl-C in an unrelated window and
' does not hold a phantom terminal for the life of the session.
'
' The host writes vision\host.log, which is where to look when it is running
' unattended and something is wrong. It retries a missing camera board forever
' rather than exiting, so starting before the board is powered is fine.

Option Explicit

Dim sh, py, script, here

here = Left(WScript.ScriptFullName, InStrRev(WScript.ScriptFullName, "\") - 1)
script = here & "\cadence_vision.py"

' The interpreter that has mediapipe installed. Change this if you move Python;
' the plain "python" on PATH may be a different install without the packages.
py = "C:\Python314\pythonw.exe"

Set sh = CreateObject("WScript.Shell")

If Not CreateObject("Scripting.FileSystemObject").FileExists(py) Then
  sh.Popup "Cadence vision host: interpreter not found at " & py, 10, _
           "Cadence", 48
  WScript.Quit 1
End If

sh.CurrentDirectory = here
sh.Run """" & py & """ """ & script & """ --host cadence-cam.local", 0, False
