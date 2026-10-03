' script.msi's VBScript custom action in the Binary table (type 6): sets a
' property, reads it back, queries the package's database and writes what
' it saw to C:\Tests\MsiOut\script-vbs.txt
Option Explicit

Function SetUp()
    Dim fso, f, back, view, rec, product, msg
    Session.Property("VBS_PROP") = "from VBScript"
    back = Session.Property("VBS_PROP")
    Set view = Session.Database.OpenView("SELECT `Value` FROM `Property` WHERE `Property` = 'ProductName'")
    view.Execute
    Set rec = view.Fetch
    If rec Is Nothing Then product = "?" Else product = rec.StringData(1)
    view.Close
    Set fso = CreateObject("Scripting.FileSystemObject")
    If Not fso.FolderExists("C:\Tests\MsiOut") Then fso.CreateFolder "C:\Tests\MsiOut"
    Set f = fso.CreateTextFile("C:\Tests\MsiOut\script-vbs.txt", True)
    f.WriteLine "VBS_PROP=" & back
    f.WriteLine "ProductName=" & UCase(product)
    f.WriteLine "Words=" & (UBound(Split(product, " ")) + 1)
    f.Close
    Set msg = Session.Installer.CreateRecord(1)
    msg.StringData(0) = "VBScript says: [1]"
    msg.StringData(1) = back
    Session.Message &H04000000, msg
    SetUp = 1
End Function
