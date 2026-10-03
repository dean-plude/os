/*
 * nstest.exe — the NetSurf browser shows SVG images, inline SVG and a
 *              script-built list, frames in an iframe and an SVG at its
 *              default size, and redraws a page a script changes after
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
 * yellow box, and at the right an SVG image without width or height (an
 * olive rectangle filling its viewBox, drawn at the default 300 x 150) and
 * an iframe showing a frameset page (a brown frame and a pink frame);
 * clicking the box runs a script that turns it green, makes it
 * bigger and adds text (an attribute and a new text node, after the page
 * was laid out).  The self-test (tests/selftest/graphics/060-nstest.py)
 * looks at the screen: the SVG's colours, the inline SVG's, the list items,
 * the default-size SVG, the two frames and the yellow box, then clicks the
 * box and waits for the green one (with the frames and images still there:
 * only the body's boxes were built again, and the iframe kept its window),
 * then closes the browser with Alt+F4.  nstest passes when
 * NetSurf exits normally (code 0) within the time limit.
 *
 * Then it opens four more pages, each with a script that changes one
 * paragraph twelve times: a long page (1,500 styled paragraphs), a page
 * with iframes below the paragraph, one with fixed, absolutely and
 * relatively positioned boxes and one with floats in boxes of their own.
 * Each page runs twice with NetSurf's layout log on
 * (NETSURF_LAYOUT_LOG): once as it is, laying the page out from the
 * changed box, and once with NETSURF_LAYOUT_CHECK=1, which follows every
 * third such layout with a full one and compares every box.  nstest
 * prints when each run's changes are done ("nstest: PAGE MODE shown"; the
 * self-test takes a screenshot of each and compares the two runs), and
 * passes a page when most of its layouts started from the changed box,
 * every check found no difference and, on the long page, the layout from
 * the changed box took less than half the full layout's time (both
 * printed).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char SVG[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"120\" viewBox=\"0 0 100 60\">\n"
    "  <rect x=\"5\" y=\"5\" width=\"40\" height=\"50\" fill=\"#0000ff\" stroke=\"#00a000\" stroke-width=\"3\"/>\n"
    "  <circle cx=\"75\" cy=\"30\" r=\"20\" fill=\"#ff0000\"/>\n"
    "</svg>\n";

/* no width or height: drawn at the default 300 x 150 (the viewBox's 2:1) */
static const char NOSIZE[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\">\n"
    "  <rect x=\"0\" y=\"0\" width=\"100\" height=\"50\" fill=\"#808000\"/>\n"
    "</svg>\n";

static const char FRAMES[] =
    "<html><head><title>frames</title></head>\n"
    "<frameset cols=\"50%,50%\"><frame src=\"left.html\"><frame src=\"right.html\"></frameset>\n"
    "</html>\n";
static const char LEFT[] = "<html><body style=\"background: #804000\"></body></html>\n";
static const char RIGHT[] = "<html><body style=\"background: #ff0080\"></body></html>\n";

static const char PAGE[] =
    "<html><head><title>nstest</title>\n"
    "<style>body { margin: 0; background: #ffffff }\n"
    "#box { background: #ffff00; width: 200px; height: 60px }</style>\n"
    "</head><body>\n"
    "<div style=\"position: absolute; left: 520px; top: 10px\">\n"
    "<img src=\"nosize.svg\"><br>\n"
    "<iframe src=\"frames.html\" width=\"300\" height=\"100\"></iframe>\n"
    "</div>\n"
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

/* Pages for the layout from a changed box (phase two): each runs a timer
 * that changes one paragraph a dozen times. */
#define CHANGES 12
#define STEP_SCRIPT(ids) \
    "<script>\n" \
    "var n = 0, ids = [" ids "];\n" \
    "var texts = ['A short line.', 'A longer paragraph that wraps over more than one line of the page: ' +\n" \
    "  'the words go on and on so that the boxes after it have to move down, and then up again when it ' +\n" \
    "  'is short once more, which is what the layout from the changed box has to get right.', 'Short again.'];\n" \
    "function step() {\n" \
    "  document.getElementById(ids[n % ids.length]).textContent = texts[n % 3] + ' (' + n + ')';\n" \
    "  if (++n < 12) setTimeout(step, 300);\n" \
    "}\n" \
    "setTimeout(step, 2000);\n" \
    "</script>\n"

static const char LONG_HEAD[] =
    "<html><head><title>long</title>\n"
    "<style>body { margin: 8px; background: #ffffff }\n"
    "p { margin: 6px 0; padding: 2px 4px; border-left: 3px solid #4060c0; font-size: 15px }\n"
    "p.odd { background: #eef0ff } h2 { margin: 12px 0 4px }</style>\n"
    "</head><body>\n<h2>A long page</h2>\n";
static const char LONG_TAIL[] = STEP_SCRIPT("'t'") "</body></html>\n";

static const char IFRAME_PAGE[] =
    "<html><head><title>iframe</title>\n"
    "<style>body { margin: 8px; background: #ffffff } p { margin: 8px 0 }</style>\n"
    "</head><body>\n"
    "<p>A page with an iframe below the paragraph a script changes.</p>\n"
    "<p id=\"t\">The paragraph a script changes.</p>\n"
    "<iframe src=\"frames.html\" width=\"400\" height=\"120\"></iframe>\n"
    "<p>A paragraph after the iframe.</p>\n"
    "<div style=\"padding: 6px; background: #e0ffe0\"><p>Inside a box with padding.</p>\n"
    "<iframe src=\"frames.html\" width=\"300\" height=\"80\"></iframe></div>\n"
    "<p>The end of the page.</p>\n"
    STEP_SCRIPT("'t'") "</body></html>\n";

static const char POSITIONED_PAGE[] =
    "<html><head><title>positioned</title>\n"
    "<style>body { margin: 8px; background: #ffffff } p { margin: 8px 0 }\n"
    "#fixed { position: fixed; top: 0; right: 0; width: 160px; height: 40px; background: #3050ff }\n"
    "#rel { position: relative; top: 10px; left: 20px; padding: 10px; background: #f0e0c0 }\n"
    "#inner { position: absolute; right: 10px; top: 5px; width: 50px; height: 50px; background: #ff3030 }\n"
    "#bottom { position: absolute; left: 400px; bottom: 30px; width: 120px; height: 30px; background: #30c030 }\n"
    "#static { position: absolute; width: 80px; height: 20px; background: #c030c0 }</style>\n"
    "</head><body>\n"
    "<div id=\"fixed\"></div>\n"
    "<p>Absolutely positioned and fixed boxes around a paragraph a script changes.</p>\n"
    "<p id=\"t\">The paragraph a script changes.</p>\n"
    "<div id=\"rel\">A relatively positioned box holding an absolutely positioned one.<div id=\"inner\"></div></div>\n"
    "<p>A paragraph with a box at its static position:</p><div id=\"static\"></div>\n"
    "<p>Another paragraph.</p><p>And one more.</p>\n"
    "<div id=\"bottom\"></div>\n"
    STEP_SCRIPT("'t'") "</body></html>\n";

static const char FLOATS_PAGE[] =
    "<html><head><title>floats</title>\n"
    "<style>body { margin: 8px; background: #ffffff } p { margin: 8px 0 }\n"
    ".bfc { overflow: hidden; background: #f4f4f4; margin: 8px 0 }\n"
    ".left { float: left; width: 150px; height: 70px; background: #ff9000; margin-right: 8px }\n"
    ".right { float: right; width: 120px; height: 50px; background: #0090ff }</style>\n"
    "</head><body>\n"
    "<p id=\"t\">A paragraph outside the floats' boxes.</p>\n"
    "<div class=\"bfc\"><div class=\"left\"></div><p id=\"u\">Text beside a float, in a box of its own.</p>\n"
    "<p>More text beside the float.</p></div>\n"
    "<p>A paragraph between the floats' boxes.</p>\n"
    "<div class=\"bfc\"><div class=\"right\"></div><p>Text beside a float on the right.</p></div>\n"
    "<p>The end of the page.</p>\n"
    STEP_SCRIPT("'t', 'u'") "</body></html>\n";

static int put(const char *path, const char *text)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    BOOL ok = WriteFile(f, text, (DWORD)strlen(text), &n, NULL) && n == strlen(text);
    CloseHandle(f);
    return ok;
}

/* the long page: 1,500 styled paragraphs, the fifth one changed */
static int put_long(const char *path)
{
    size_t cap = 300000, len = 0;
    char *buf = malloc(cap);
    int i, ok;
    if (!buf) return 0;
    len += sprintf(buf + len, "%s", LONG_HEAD);
    for (i = 0; i < 1500; i++) {
        len += sprintf(buf + len, "<p%s%s>Paragraph %d of the long page, with a few words so that it "
                       "takes up a line of its own.</p>\n", i == 4 ? " id=\"t\"" : "",
                       i % 2 ? " class=\"odd\"" : "", i + 1);
        if (len + 512 > cap) { free(buf); return 0; }
    }
    len += sprintf(buf + len, "%s", LONG_TAIL);
    ok = put(path, buf);
    free(buf);
    return ok;
}

static unsigned num_after(const char *line, const char *key)
{
    const char *p = strstr(line, key);
    return p ? (unsigned)strtoul(p + strlen(key), NULL, 10) : 0;
}

/* what a run's log says */
struct runlog {
    int relayouts, partial, checks, differ;
    double us, us_flow, us_place;           /* from the changed box: sums */
    double full_us, full_flow, full_place;  /* the whole page (check lines): sums */
    char first[300];
};

static void read_log(const char *path, struct runlog *r)
{
    char line[1024];
    FILE *f = fopen(path, "r");
    memset(r, 0, sizeof(*r));
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "relayout:", 9)) {
            r->relayouts++;
            if (strstr(line, "(from the changed box")) {
                r->partial++;
                r->us += num_after(line, ", layout ");
                r->us_flow += num_after(line, "flow ");
                r->us_place += num_after(line, "placing ");
            }
        } else if (!strncmp(line, "check:", 6)) {
            r->checks++;
            r->full_us += num_after(line, "whole page ");
            r->full_flow += num_after(line, "flow ");
            r->full_place += num_after(line, "placing ");
            if (!strstr(line, " 0 differ")) {
                if (!r->differ) {
                    strncpy(r->first, line, sizeof(r->first) - 1);
                    r->first[strcspn(r->first, "\r\n")] = 0;
                }
                r->differ++;
            }
        }
    }
    fclose(f);
}

/* starts NetSurf on @page with the layout log on (and the check), waits
 * for the script's changes to end, lets the test take a screenshot and
 * closes NetSurf */
static int run_page(const char *page, int check, struct runlog *r)
{
    char log[MAX_PATH], cmd[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int last = -1, still = 0, waited = 0;

    sprintf(log, "C:\\Temp\\nstest\\%s-%s.log", page, check ? "check" : "incremental");
    DeleteFileA(log);
    SetEnvironmentVariableA("NETSURF_LAYOUT_LOG", log);
    SetEnvironmentVariableA("NETSURF_LAYOUT_CHECK", check ? "1" : NULL);
    sprintf(cmd, "C:\\Programs\\NetSurf\\netsurf.exe file:///Temp/nstest/%s.html", page);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("FAIL: could not start NetSurf on %s (error %lu)\n", page, GetLastError());
        return 0;
    }
    /* the script makes CHANGES changes; wait until the log has them all,
     * or has not grown for 10 s */
    while (waited < 300000) {
        read_log(log, r);
        if (r->relayouts >= CHANGES) break;
        if (r->relayouts == last && r->relayouts > 0 && ++still >= 40) break;
        if (r->relayouts != last) still = 0;
        last = r->relayouts;
        Sleep(250);
        waited += 250;
    }
    Sleep(1500);
    printf("nstest: %s %s shown\n", page, check ? "check" : "incremental");
    fflush(stdout);
    Sleep(6000);                            /* (the test's screenshot) */
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    read_log(log, r);
    return 1;
}

/* phase two: the layout from a changed box on four pages, each against a
 * whole-page layout box by box (NETSURF_LAYOUT_CHECK) */
static void phase_two(int *pass, int *fail)
{
    static const char *pages[] = { "long", "iframe", "positioned", "floats" };
    int i;
    for (i = 0; i < 4; i++) {
        struct runlog inc, chk;
        if (!run_page(pages[i], 0, &inc) || !run_page(pages[i], 1, &chk)) {
            (*fail)++;
            continue;
        }
        printf("nstest: %s: %d relayouts, %d from the changed box; check: %d compared, %d differ\n",
               pages[i], inc.relayouts, inc.partial, chk.checks, chk.differ);
        if (!strcmp(pages[i], "long") && inc.partial && chk.checks)
            printf("nstest: long page layout after a change: %.1f ms from the changed box "
                   "(flow %.1f ms, placing %.1f ms), %.1f ms for the whole page "
                   "(flow %.1f ms, placing %.1f ms)\n",
                   inc.us / inc.partial / 1000, inc.us_flow / inc.partial / 1000,
                   inc.us_place / inc.partial / 1000, chk.full_us / chk.checks / 1000,
                   chk.full_flow / chk.checks / 1000, chk.full_place / chk.checks / 1000);
        if (inc.relayouts == 0) {
            printf("FAIL: %s: the script's changes were not laid out\n", pages[i]);
            (*fail)++;
        } else if (inc.partial * 2 < inc.relayouts) {
            printf("FAIL: %s: only %d of %d layouts started from the changed box\n",
                   pages[i], inc.partial, inc.relayouts);
            (*fail)++;
        } else if (chk.checks == 0) {
            printf("FAIL: %s: no layout from the changed box was checked\n", pages[i]);
            (*fail)++;
        } else if (chk.differ) {
            printf("FAIL: %s: %d layouts differ from the whole page's: %s\n", pages[i], chk.differ, chk.first);
            (*fail)++;
        } else if (!strcmp(pages[i], "long") &&
                   inc.us / inc.partial * 2 > chk.full_us / chk.checks) {
            printf("FAIL: long: the layout from the changed box (%.1f ms) is not twice as fast as "
                   "the whole page's (%.1f ms)\n", inc.us / inc.partial / 1000,
                   chk.full_us / chk.checks / 1000);
            (*fail)++;
        } else {
            (*pass)++;
        }
    }
}

int main(void)
{
    int pass = 0, fail = 0;
    CreateDirectoryA("C:\\Temp", NULL);
    CreateDirectoryA("C:\\Temp\\nstest", NULL);
    if (!put("C:\\Temp\\nstest\\shape.svg", SVG) || !put("C:\\Temp\\nstest\\page.html", PAGE) ||
        !put("C:\\Temp\\nstest\\nosize.svg", NOSIZE) || !put("C:\\Temp\\nstest\\frames.html", FRAMES) ||
        !put("C:\\Temp\\nstest\\left.html", LEFT) || !put("C:\\Temp\\nstest\\right.html", RIGHT) ||
        !put_long("C:\\Temp\\nstest\\long.html") || !put("C:\\Temp\\nstest\\iframe.html", IFRAME_PAGE) ||
        !put("C:\\Temp\\nstest\\positioned.html", POSITIONED_PAGE) ||
        !put("C:\\Temp\\nstest\\floats.html", FLOATS_PAGE)) {
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

    phase_two(&pass, &fail);
    printf("nstest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
