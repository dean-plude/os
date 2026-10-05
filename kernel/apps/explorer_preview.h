/* Native selection preview. The Explorer painter already holds FsLock;
 * reuse the file-icon cache rather than decoding a second thumbnail. */
#pragma once

static void preview_text(int x, int y, int width, const char *text, bool bold)
{
    char label[RAMFS_NAME_MAX + 4];
    strncpy(label, text, sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    int (*measure)(const char *) = bold ? GdiTextBoldW : GdiTextW;
    if (measure(label) > width) {
        int n = (int)strlen(label);
        while (n && measure(label) + GdiTextW("...") > width) label[--n] = '\0';
        strcat(label, "...");
    }
    if (bold) GdiTextBold(x, y, label, UI_TEXT);
    else GdiTextT(x, y, label, UI_TEXT2);
}

static void draw_preview(WND *w, Explorer *e, GdiRect c)
{
    GdiRect pane = off(r_preview(c), c);
    GdiSetClip(pane);
    GdiAlphaFill(pane, GDI_C(23, 28, 44), 135);
    GdiAlphaFill(RECT(pane.x, pane.y, 1, pane.h), GDI_WHITE, 30);
    int x = pane.x + 16, y = pane.y + 18, width = pane.w - 32;
    GdiTextBold(x, y, "Preview", UI_TEXT);
    RamNode *file = row_at(e, e->sel);
    if (!file) {
        AppDrawGlyph(GL_FILE, x + (width - 48) / 2, y + 56, 48, UI_TEXT3);
        GdiTextCenter(x, y + 134, width, "Select an item", UI_TEXT2);
        GdiTextCenter(x, y + 154, width, "to see its details.", UI_TEXT3);
        GdiSetClip(c);
        return;
    }
    GdiRect image = RECT(x, y + 34, width, 126);
    GdiRoundAlpha(image, 10, GDI_WHITE, 12);
    GdiIcon *icon = !file->dir ? AppFileIcon(file) : NULL;
    if (!IconDrawFit(icon, 0, image)) {
        if (file->dir) AppDrawFolderKindIcon(AppFolderKind(file), x + (width - 72) / 2, image.y + 26, 72);
        else AppDrawFileIcon(x + (width - 64) / 2, image.y + 30, 64);
    }
    preview_text(x, y + 174, width, file->name, true);
    preview_text(x, y + 198, width, file->dir ? "Folder" : AppFileTypeName(file), false);
    char size[32];
    if (file->dir) ksnprintf(size, sizeof(size), "%d items", RamfsCount(file));
    else AppFormatSize(file->size, size, sizeof(size));
    GdiTextT(x, y + 220, size, UI_TEXT2);
    if (icon && icon->n && icon->img[0].png && icon->img[0].px) {
        char dimensions[40];
        ksnprintf(dimensions, sizeof(dimensions), "%d x %d pixels", icon->img[0].pw, icon->img[0].ph);
        GdiTextT(x, y + 242, dimensions, UI_TEXT2);
    }
    /* Native operations, not the prototype's simulated share toast. */
    UiButton(off(r_preview_open(c), c), "Open", true);
    UiButton(off(r_preview_copy(c), c), "Copy path", false);
    GdiSetClip(c);
}

/* Return true only for a preview command; list and toolbar clicks keep
 * their existing routes. Called on release, like the command bar. */
static bool preview_action(WND *w, Explorer *e, GdiRect c, int x, int y)
{
    if (!preview_width(c)) return false;
    if (UiHit(r_preview_open(c), x, y)) { open_sel(w, e); return true; }
    if (!UiHit(r_preview_copy(c), x, y)) return false;
    RamNode *node = row_at(e, e->sel);
    if (node) {
        char path[RAMFS_PATH_MAX];
        RamfsPath(node, path, sizeof(path));
        ClipSetText(path, (UINT32)strlen(path));
        ksnprintf(e->status, sizeof(e->status), "Path copied");
    }
    return true;
}
