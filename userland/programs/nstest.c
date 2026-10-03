/*
 * nstest.exe — the NetSurf browser shows SVG images, inline SVG and a
 *              script-built list, and redraws a page a script changes after
 *              layout (Phase 19.8)
 *
 *   nstest      writes a test page and an SVG image to C:\Temp\nstest,
 *               starts C:\Programs\NetSurf\netsurf.exe on the page and
 *               waits for the browser to quit
 *
 * The page shows the SVG (a blue rectangle with a green outline and a red
 * circle, drawn by NetSurf's path plotter), an svg element written inline
 * in the HTML (a magenta square and a cyan circle, its viewBox scaled 4x),
 * a list a script builds (three items in orange, purple and teal) and a
 * yellow box; clicking the box runs a script that turns it green, makes it
 * bigger and adds text (an attribute and a new text node, after the page
 * was laid out).  The self-test (tests/selftest/graphics/060-nstest.py)
 * looks at the screen: the SVG's colours, the inline SVG's, the list items
 * and the yellow box, then clicks the box and waits for the green one,
 * then closes the browser with Alt+F4.  nstest passes when
 * NetSurf exits normally (code 0) within the time limit.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char SVG[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"120\" viewBox=\"0 0 100 60\">\n"
    "  <rect x=\"5\" y=\"5\" width=\"40\" height=\"50\" fill=\"#0000ff\" stroke=\"#00a000\" stroke-width=\"3\"/>\n"
    "  <circle cx=\"75\" cy=\"30\" r=\"20\" fill=\"#ff0000\"/>\n"
    "</svg>\n";

static const char PAGE[] =
    "<html><head><title>nstest</title>\n"
    "<style>body { margin: 0; background: #ffffff }\n"
    "#box { background: #ffff00; width: 200px; height: 60px }</style>\n"
    "</head><body>\n"
    "<p>An SVG image:</p>\n"
    "<img src=\"shape.svg\" width=\"200\" height=\"120\">\n"
    "<p>Inline SVG:</p>\n"
    "<svg width=\"160\" height=\"80\" viewBox=\"0 0 40 20\">\n"
    "  <rect x=\"0\" y=\"0\" width=\"20\" height=\"20\" fill=\"#ff00ff\"/>\n"
    "  <circle cx=\"30\" cy=\"10\" r=\"8\" fill=\"#00ffff\"/>\n"
    "</svg>\n"
    "<ul id=\"list\"></ul>\n"
    "<script>\n"
    "var colours = ['#ff8000', '#8000ff', '#008080'];\n"
    "for (var i = 0; i < colours.length; i++) {\n"
    "  var li = document.createElement('li');\n"
    "  li.setAttribute('style', 'background: ' + colours[i] + '; width: 100px; height: 20px');\n"
    "  document.getElementById('list').appendChild(li);\n"
    "}\n"
    "</script>\n"
    "<div id=\"box\" onclick=\"change()\">click me</div>\n"
    "<script>\n"
    "function change() {\n"
    "  var box = document.getElementById('box');\n"
    "  box.setAttribute('style', 'background: #00ff00; width: 300px; height: 80px');\n"
    "  box.appendChild(document.createTextNode(' changed by a script'));\n"
    "}\n"
    "</script>\n"
    "</body></html>\n";

static int put(const char *path, const char *text)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    BOOL ok = WriteFile(f, text, (DWORD)strlen(text), &n, NULL) && n == strlen(text);
    CloseHandle(f);
    return ok;
}

int main(void)
{
    int pass = 0, fail = 0;
    CreateDirectoryA("C:\\Temp", NULL);
    CreateDirectoryA("C:\\Temp\\nstest", NULL);
    if (!put("C:\\Temp\\nstest\\shape.svg", SVG) || !put("C:\\Temp\\nstest\\page.html", PAGE)) {
        printf("FAIL: could not write the test page to C:\\Temp\\nstest\n");
        return 1;
    }

    char cmd[] = "C:\\Programs\\NetSurf\\netsurf.exe file:///Temp/nstest/page.html";
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL: could not start NetSurf (error %lu)\n", GetLastError());
        return 1;
    }
    printf("nstest: NetSurf shows the test page\n");
    fflush(stdout);

    /* the test checks the screen, clicks the box and closes NetSurf */
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 600000) != WAIT_OBJECT_0) {
        printf("FAIL: NetSurf did not quit\n");
        TerminateProcess(pi.hProcess, 1);
        fail++;
    } else if (!GetExitCodeProcess(pi.hProcess, &code) || code != 0) {
        printf("FAIL: NetSurf exited with code %lu\n", code);
        fail++;
    } else {
        pass++;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("nstest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
