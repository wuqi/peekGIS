' peekGIS launcher (flash-free): runs the built binary without opening a console.
' This file lives in launcher/, the repo root is one level above.
' Usage: double-click this file, or run:  wscript.exe launcher\peekgis.vbs
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh  = CreateObject("WScript.Shell")
here = fso.GetParentFolderName(WScript.ScriptFullName)
exeDir = fso.GetAbsolutePathName(here & "\..\build\windows\x64\release\bin")
If Not fso.FileExists(exeDir & "\peekgis.exe") Then
    MsgBox "peekgis.exe not found, build first:  xmake build peekgis" & vbCrLf & exeDir, 48, "peekGIS"
    WScript.Quit 1
End If
sh.CurrentDirectory = exeDir
sh.Run """" & exeDir & "\peekgis.exe""", 0, False   ' 0 = hidden window