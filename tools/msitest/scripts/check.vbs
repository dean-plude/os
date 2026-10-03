' script.msi's VBScript in an installed file (type 22), deferred: its
' CustomActionData is "folder|value"; it also writes the registry with
' WScript.Shell and reads it back
Function Check()
    Dim data, sh
    data = Split(Session.Property("CustomActionData"), "|")
    Set sh = CreateObject("WScript.Shell")
    sh.RegWrite "HKLM\SOFTWARE\NovaScriptShell\FromVbsFile", data(1)
    With CreateObject("Scripting.FileSystemObject").OpenTextFile(data(0) & "script-vbsfile.txt", 2, True)
        .Write "CustomActionData=" & data(1) & " shell=" & sh.RegRead("HKLM\SOFTWARE\NovaScriptShell\FromVbsFile")
        .Close
    End With
    Check = 1
End Function
