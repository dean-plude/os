// script.msi's JScript in an installed file (type 21), deferred: its
// CustomActionData is "folder|value"
function Check() {
    var data = Session.Property("CustomActionData").split("|");
    var fso = new ActiveXObject("Scripting.FileSystemObject");
    var f = fso.OpenTextFile(data[0] + "script-jsfile.txt", 2, true);
    f.Write("CustomActionData=" + data[1]);
    f.Close();
    return fso.FileExists(data[0] + "script-jsfile.txt") ? 1 : 3;
}
