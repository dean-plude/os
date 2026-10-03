// script.msi's JScript custom action in the Binary table (type 5): sets a
// property, reads it back, queries the package's database and writes what
// it saw to C:\Tests\MsiOut\script-js.txt
function SetUp() {
    Session.Property("JS_PROP") = "from JScript";
    var back = Session.Property("JS_PROP");
    var view = Session.Database.OpenView("SELECT `Value` FROM `Property` WHERE `Property` = 'ProductName'");
    view.Execute();
    var rec = view.Fetch();
    var product = rec ? rec.StringData(1) : "?";
    view.Close();
    var fso = new ActiveXObject("Scripting.FileSystemObject");
    if (!fso.FolderExists("C:\\Tests\\MsiOut")) fso.CreateFolder("C:\\Tests\\MsiOut");
    var f = fso.CreateTextFile("C:\\Tests\\MsiOut\\script-js.txt", true);
    f.WriteLine("JS_PROP=" + back);
    f.WriteLine("ProductName=" + product);
    f.WriteLine("INSTALLDIR=" + Session.TargetPath("INSTALLDIR"));
    f.WriteLine("Complete=" + Session.FeatureRequestState("Complete"));
    f.Close();
    var log = Session.Installer.CreateRecord(1);
    log.StringData(0) = "JScript says: [1]";
    log.StringData(1) = back;
    Session.Message(0x04000000, log);
    return 1;
}
