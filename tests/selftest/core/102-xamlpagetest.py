# xamlpagetest: a XAML page in a Window (roadmap step 32.4, first part): an app
# started by Application.Start makes a Window in OnLaunched (DesktopWindowImpl,
# its window chrome, DesktopWindowXamlSource and XAML island root, on NovaOS's
# lifted content islands in Microsoft.UI.Input.dll), sets a page loaded with
# XamlReader.Load as its content and activates it; the page is laid out and
# drawn in the window (screenshot), then the window closes and the app exits
DOC = ('`xamlpagetest` (a XAML page in a `Window`: the framework\'s desktop window, built from its sources, on the '
       'Windows App SDK\'s content islands as NovaOS\'s `Microsoft.UI.Input.dll` serves them (`ContentIsland`, '
       '`DesktopChildSiteBridge`, the island\'s input sources, `AppWindow`), showing a `Grid` with a `TextBlock` and a '
       '`Button` loaded by `XamlReader.Load`, laid out to the window\'s size and drawn through the framework\'s composition '
       'tree on `dcomp.dll`\'s compositor, then closed)')
TESTS = [
    Test('xamlpagetest', 'xamlpagetest', [r'xamlpagetest: \d+ passed, 0 failed \(64-bit\)'], timeout=200,
         shot=r'xamlpagetest: page on screen'),
]
