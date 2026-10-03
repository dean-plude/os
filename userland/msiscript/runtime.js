/*
 * runtime.js: what a script custom action sees, for JScript and VBScript
 * alike.  The objects Windows Installer and Windows Script Host give
 * scripts (Session, Installer, Database, View, Record,
 * Scripting.FileSystemObject, Scripting.Dictionary, WScript.Shell) are
 * written here on __nova, the few native calls msiscript.c provides.
 *
 * Members keep Microsoft's names.  A parametrized property is a method
 * (Session.Property("X")); setting one, Session.Property("X") = "1", calls
 * put_Property("X", "1") through __put (mujs compiles that assignment so
 * in JScript; translated VBScript calls __put itself).
 */

/* JScript has String.prototype.substr; mujs (ES5) does not */
if (!String.prototype.substr) {
    Object.defineProperty(String.prototype, 'substr', {
        value: function (start, n) {
            var s = String(this);
            start = start < 0 ? Math.max(s.length + start, 0) : start | 0;
            return s.substring(start, n === undefined ? s.length : start + Math.max(n | 0, 0));
        },
        writable: true, configurable: true
    });
}

function __error(number, text) {
    var e = new Error(text);
    e.number = number;
    return e;
}

/* an MSI call that failed: as Windows reports it, "Session.Property" */
function __msierr(what) {
    return __error(-2147023170 /* 0x800706BE */, what + ' failed');
}

function __getter(proto, name, get, set) {
    var d = { get: get, enumerable: true, configurable: true };
    if (set) d.set = set;
    Object.defineProperty(proto, name, d);
}

function __s(v) {
    if (v === undefined || v === null) return '';
    if (v === true) return 'True';
    if (v === false) return 'False';
    if (v === __vbnull) return '';
    return String(v);
}

/* ---------------------------------------------------------------------
 * Windows Installer: Session, Installer, Database, View, Record
 * --------------------------------------------------------------------- */
function __Record(h) { this.__h = h; }
__Record.prototype = {
    StringData: function (i) { return __nova.rec_get(this.__h, i); },
    put_StringData: function (i, v) { __nova.rec_set(this.__h, i, __s(v)); },
    IntegerData: function (i) { return __nova.rec_getint(this.__h, i); },
    put_IntegerData: function (i, v) { __nova.rec_setint(this.__h, i, Number(v) | 0); },
    IsNull: function (i) { return __nova.rec_isnull(this.__h, i); },
    DataSize: function (i) { return __nova.rec_size(this.__h, i); },
    ClearData: function () { __nova.rec_clear(this.__h); },
    FormatText: function () { return __nova.format(0, this.__h); },
    ReadStream: function () { return ''; },
    SetStream: function () { throw __msierr('Record.SetStream'); }
};
__getter(__Record.prototype, 'FieldCount', function () { return __nova.rec_count(this.__h); });

function __record(h) { return h ? new __Record(h) : null; }
function __rh(r) { return r ? r.__h : 0; }

function __View(h) { this.__h = h; }
__View.prototype = {
    Execute: function (rec) { __nova.view_execute(this.__h, __rh(rec)); },
    Fetch: function () { return __record(__nova.view_fetch(this.__h)); },
    Modify: function (mode, rec) { __nova.view_modify(this.__h, mode, __rh(rec)); },
    ColumnInfo: function (type) { return __record(__nova.view_colinfo(this.__h, type)); },
    Close: function () { __nova.view_close(this.__h); }
};

function __Database(h) { this.__h = h; }
__Database.prototype = {
    OpenView: function (sql) { return new __View(__nova.open_view(this.__h, __s(sql))); },
    Commit: function () { __nova.db_commit(this.__h); },
    PrimaryKeys: function (table) { return __record(__nova.primary_keys(this.__h, __s(table))); },
    TablePersistent: function (table) { return 1; }
};
__getter(__Database.prototype, 'DatabaseState', function () { return __nova.db_state(this.__h); });

/* MsiDoAction's result as a msiDoActionStatus */
function __status(r) {
    if (r === 0) return 1;                 /* msiDoActionStatusSuccess */
    if (r === 1602) return 2;              /* UserExit */
    if (r === 1626 || r === 259) return 0; /* NoAction */
    return 3;                              /* Failure */
}

function __Session(h) { this.__h = h; }
__Session.prototype = {
    Property: function (name) { return __nova.prop_get(this.__h, __s(name)); },
    put_Property: function (name, v) { __nova.prop_set(this.__h, __s(name), __s(v)); },
    Mode: function (m) { return __nova.mode_get(this.__h, m); },
    put_Mode: function (m, v) { __nova.mode_set(this.__h, m, __cond(v)); },
    TargetPath: function (dir) { return __nova.target_get(this.__h, __s(dir)); },
    put_TargetPath: function (dir, path) { __nova.target_set(this.__h, __s(dir), __s(path)); },
    SourcePath: function (dir) { return __nova.source_get(this.__h, __s(dir)); },
    FeatureCurrentState: function (f) { return __nova.feature_state(this.__h, __s(f))[0]; },
    FeatureRequestState: function (f) { return __nova.feature_state(this.__h, __s(f))[1]; },
    put_FeatureRequestState: function (f, s) { __nova.feature_set(this.__h, __s(f), Number(s)); },
    FeatureValidStates: function (f) { __nova.feature_state(this.__h, __s(f)); return 0x1C; },
    ComponentCurrentState: function (c) { return __nova.comp_state(this.__h, __s(c))[0]; },
    ComponentRequestState: function (c) { return __nova.comp_state(this.__h, __s(c))[1]; },
    put_ComponentRequestState: function (c, s) { __nova.comp_set(this.__h, __s(c), Number(s)); },
    EvaluateCondition: function (c) { return __nova.eval_cond(this.__h, __s(c)); },
    FormatRecord: function (rec) { return __nova.format(this.__h, __rh(rec)); },
    Message: function (kind, rec) { return __nova.message(this.__h, Number(kind), __rh(rec)); },
    DoAction: function (a) { return __status(__nova.do_action(this.__h, __s(a))); },
    Sequence: function (t) { return __status(__nova.sequence(this.__h, __s(t))); },
    SetInstallLevel: function (l) { __nova.set_level(this.__h, Number(l)); }
};
__getter(__Session.prototype, 'Installer', function () { return __installer; });
__getter(__Session.prototype, 'Database', function () { return new __Database(__nova.active_db(this.__h)); });
__getter(__Session.prototype, 'Language', function () { return __nova.language(this.__h); });

/* registry roots by name and by Windows Installer's numbers */
var __roots = {
    hkey_classes_root: 0, hkcr: 0, hkey_current_user: 1, hkcu: 1, hkey_local_machine: 2, hklm: 2,
    hkey_users: 3, hku: 3, hkey_current_config: 5, hkcc: 5
};

function __Installer() {}
__Installer.prototype = {
    CreateRecord: function (n) { return new __Record(__nova.rec_create(Number(n))); },
    OpenDatabase: function (path, mode) {
        return new __Database(__nova.open_db(__s(path), typeof mode === 'number' ? mode : 0));
    },
    Environment: function (name) { return __nova.env_get(__s(name)); },
    put_Environment: function (name, v) { __nova.env_set(__s(name), __s(v)); },
    FileAttributes: function (path) { return __nova.attr(__s(path)); },
    FileSize: function (path) { return __nova.size(__s(path)); },
    FileVersion: function (path) { return __nova.version(__s(path)); },
    RegistryValue: function (root, key, value) {
        if (typeof root === 'string') root = __roots[root.toLowerCase()];
        if (value === undefined) return __nova.reg_key(Number(root) & 0xF, __s(key));   /* "is the key there" */
        var v = __nova.reg_read(Number(root) & 0xF, __s(key), __s(value));
        return v === undefined ? '' : (typeof v === 'number' ? '#' + v : v);
    },
    ProductState: function (code) { return __nova.product_state(__s(code)); },
    LastErrorRecord: function () { return null; }
};
__getter(__Installer.prototype, 'UILevel', function () { return 2; });
var __installer = new __Installer();

/* ---------------------------------------------------------------------
 * Collections: Count, Item, and enumerable with For Each / Enumerator
 * --------------------------------------------------------------------- */
function __Collection(items, names) { this.__a = items; this.__n = names; }
__Collection.prototype = {
    Item: function (k) {
        if (typeof k === 'number') return this.__a[k];
        var lk = __s(k).toLowerCase();
        for (var i = 0; i < this.__a.length; i++) if (this.__n[i].toLowerCase() === lk) return this.__a[i];
        throw __error(5, 'Invalid procedure call or argument');
    }
};
__getter(__Collection.prototype, 'Count', function () { return this.__a.length; });

/* what For Each walks */
function __each(c) {
    if (c instanceof Array) return c;
    if (c && c.__a) return c.__a;
    if (c && c.__keys) return c.Keys();
    throw __error(451, 'Object not a collection');
}

function Enumerator(c) { this.__x = c === undefined ? [] : __each(c); this.__i = 0; }
Enumerator.prototype = {
    atEnd: function () { return this.__i >= this.__x.length; },
    moveNext: function () { this.__i++; },
    moveFirst: function () { this.__i = 0; },
    item: function () { return this.__x[this.__i]; }
};

/* ---------------------------------------------------------------------
 * Scripting.FileSystemObject
 * --------------------------------------------------------------------- */
function __slash(p) { return p.replace(/\//g, '\\'); }
function __name(p) { p = __slash(p).replace(/\\+$/, ''); var i = p.lastIndexOf('\\'); return i < 0 ? p : p.substr(i + 1); }
function __parent(p) {
    p = __slash(p).replace(/\\+$/, '');
    var i = p.lastIndexOf('\\');
    if (i < 0) return '';
    return i === 2 && p.charAt(1) === ':' ? p.substr(0, 3) : p.substr(0, i);
}
function __notfound(what) { return __error(53, 'File not found'); }

function __TextStream(path, mode, text) {
    this.__p = path; this.__m = mode; this.__t = text || ''; this.__i = 0; this.__line = 1;
}
__TextStream.prototype = {
    __w: function (s) {
        if (this.__m === 1) throw __error(54, 'Bad file mode');
        if (!__nova.file_write(this.__p, s, true)) throw __error(70, 'Permission denied');
        for (var i = 0; i < s.length; i++) if (s.charAt(i) === '\n') this.__line++;
    },
    __r: function () { if (this.__m !== 1) throw __error(54, 'Bad file mode'); },
    Write: function (s) { this.__w(__s(s)); },
    WriteLine: function (s) { this.__w(__s(s) + '\r\n'); },
    WriteBlankLines: function (n) { var s = ''; for (var i = 0; i < n; i++) s += '\r\n'; this.__w(s); },
    ReadAll: function () { this.__r(); var s = this.__t.substr(this.__i); this.__i = this.__t.length; return s; },
    ReadLine: function () {
        this.__r();
        if (this.__i >= this.__t.length) throw __error(62, 'Input past end of file');
        var j = this.__t.indexOf('\n', this.__i);
        if (j < 0) j = this.__t.length;
        var s = this.__t.substring(this.__i, j).replace(/\r$/, '');
        this.__i = j + 1;
        this.__line++;
        return s;
    },
    Read: function (n) { this.__r(); var s = this.__t.substr(this.__i, n); this.__i += s.length; return s; },
    SkipLine: function () { this.ReadLine(); },
    Skip: function (n) { this.__i += n; },
    Close: function () {}
};
__getter(__TextStream.prototype, 'AtEndOfStream', function () { return this.__i >= this.__t.length; });
__getter(__TextStream.prototype, 'AtEndOfLine', function () {
    return this.__i >= this.__t.length || this.__t.charAt(this.__i) === '\r' || this.__t.charAt(this.__i) === '\n';
});
__getter(__TextStream.prototype, 'Line', function () { return this.__line; });

/* mode: 1 reading, 2 writing, 8 appending */
function __open(path, mode, create) {
    path = __slash(__s(path));
    var exists = __nova.attr(path) >= 0;
    if (mode === 1) {
        var t = __nova.file_read(path);
        if (t === null) throw __notfound();
        return new __TextStream(path, 1, t);
    }
    if (!exists && !create) throw __notfound();
    if (mode === 2 || !exists) if (!__nova.file_write(path, '', false)) throw __error(76, 'Path not found');
    return new __TextStream(path, mode);
}

function __File(path) { this.__p = path; }
__File.prototype = {
    Delete: function () { if (!__nova.rmfile(this.__p)) throw __error(70, 'Permission denied'); },
    Copy: function (dst, over) { __fso.CopyFile(this.__p, dst, over); },
    Move: function (dst) { __fso.MoveFile(this.__p, dst); this.__p = __slash(__s(dst)); },
    OpenAsTextStream: function (mode) { return __open(this.__p, mode || 1, false); }
};
__getter(__File.prototype, 'Path', function () { return this.__p; });
__getter(__File.prototype, 'Name', function () { return __name(this.__p); });
__getter(__File.prototype, 'ShortName', function () { return __name(this.__p); });
__getter(__File.prototype, 'ShortPath', function () { return this.__p; });
__getter(__File.prototype, 'Size', function () { return __nova.size(this.__p); });
__getter(__File.prototype, 'Attributes', function () { return __nova.attr(this.__p) & 0x37; });
__getter(__File.prototype, 'Type', function () { return 'File'; });
__getter(__File.prototype, 'ParentFolder', function () { return new __Folder(__parent(this.__p)); });

function __Folder(path) { this.__p = path; }
__Folder.prototype = {
    Delete: function () { if (!__nova.rmdir(this.__p, true)) throw __error(70, 'Permission denied'); },
    Copy: function (dst) { __fso.CopyFolder(this.__p, dst); },
    CreateTextFile: function (name, over) { return __fso.CreateTextFile(this.__p + '\\' + name, over); }
};
__getter(__Folder.prototype, 'Path', function () { return this.__p; });
__getter(__Folder.prototype, 'Name', function () { return __name(this.__p); });
__getter(__Folder.prototype, 'ShortPath', function () { return this.__p; });
__getter(__Folder.prototype, 'Attributes', function () { return __nova.attr(this.__p) & 0x37; });
__getter(__Folder.prototype, 'IsRootFolder', function () { return /^[A-Za-z]:\\?$/.test(this.__p); });
__getter(__Folder.prototype, 'ParentFolder', function () { var p = __parent(this.__p); return p ? new __Folder(p) : null; });
__getter(__Folder.prototype, 'Size', function () { return 0; });
function __listing(dir, dirs, make) {
    var names = __nova.list(dir, dirs), a = [];
    for (var i = 0; i < names.length; i++) a.push(make(dir.replace(/\\$/, '') + '\\' + names[i]));
    return new __Collection(a, names);
}
__getter(__Folder.prototype, 'Files', function () { return __listing(this.__p, false, function (p) { return new __File(p); }); });
__getter(__Folder.prototype, 'SubFolders', function () { return __listing(this.__p, true, function (p) { return new __Folder(p); }); });

function __FileSystemObject() {}
__FileSystemObject.prototype = {
    FileExists: function (p) { var a = __nova.attr(__slash(__s(p))); return a >= 0 && !(a & 16); },
    FolderExists: function (p) { var a = __nova.attr(__slash(__s(p))); return a >= 0 && !!(a & 16); },
    DriveExists: function (d) { return __nova.attr(__s(d).substr(0, 1) + ':\\') >= 0; },
    CreateFolder: function (p) {
        p = __slash(__s(p));
        if (__nova.attr(p) >= 0) throw __error(58, 'File already exists');
        if (!__nova.mkdir(p)) throw __error(76, 'Path not found');
        return new __Folder(p);
    },
    DeleteFile: function (p) { if (!__nova.rmfile(__slash(__s(p)))) throw __notfound(); },
    DeleteFolder: function (p) { if (!__nova.rmdir(__slash(__s(p)), true)) throw __error(76, 'Path not found'); },
    CopyFile: function (src, dst, over) {
        src = __slash(__s(src)); dst = __slash(__s(dst));
        if (/\\$/.test(dst)) dst += __name(src);
        else if (this.FolderExists(dst)) dst += '\\' + __name(src);
        if (!__nova.copy(src, dst, over === undefined ? true : __cond(over))) throw __error(58, 'File already exists');
    },
    MoveFile: function (src, dst) {
        src = __slash(__s(src)); dst = __slash(__s(dst));
        if (/\\$/.test(dst) || this.FolderExists(dst)) dst = dst.replace(/\\$/, '') + '\\' + __name(src);
        if (!__nova.move(src, dst)) throw __error(58, 'File already exists');
    },
    CopyFolder: function (src, dst) {
        src = __slash(__s(src)); dst = __slash(__s(dst));
        if (__nova.attr(dst) < 0) __nova.mkdir(dst);
        var f = __nova.list(src, false), d = __nova.list(src, true), i;
        for (i = 0; i < f.length; i++) __nova.copy(src + '\\' + f[i], dst + '\\' + f[i], true);
        for (i = 0; i < d.length; i++) this.CopyFolder(src + '\\' + d[i], dst + '\\' + d[i]);
    },
    MoveFolder: function (src, dst) { if (!__nova.move(__slash(__s(src)), __slash(__s(dst)))) throw __error(58, 'File already exists'); },
    GetFile: function (p) { p = __nova.full(__slash(__s(p))); if (!this.FileExists(p)) throw __notfound(); return new __File(p); },
    GetFolder: function (p) { p = __nova.full(__slash(__s(p))); if (!this.FolderExists(p)) throw __error(76, 'Path not found'); return new __Folder(p); },
    GetSpecialFolder: function (n) { return new __Folder(__nova.special(Number(n))); },
    GetTempName: function () { return 'rad' + Math.floor(Math.random() * 0xFFFFF).toString(16).toUpperCase() + '.tmp'; },
    BuildPath: function (p, n) {
        p = __s(p); n = __s(n);
        return p === '' || /[\\:]$/.test(p) ? p + n : p + '\\' + n;
    },
    GetFileName: function (p) { return __name(__s(p)); },
    GetBaseName: function (p) { var n = __name(__s(p)), i = n.lastIndexOf('.'); return i < 0 ? n : n.substr(0, i); },
    GetExtensionName: function (p) { var n = __name(__s(p)), i = n.lastIndexOf('.'); return i < 0 ? '' : n.substr(i + 1); },
    GetParentFolderName: function (p) { return __parent(__s(p)); },
    GetDriveName: function (p) { var m = /^[A-Za-z]:/.exec(__s(p)); return m ? m[0] : ''; },
    GetAbsolutePathName: function (p) { return __nova.full(__slash(__s(p))); },
    CreateTextFile: function (p, over) {
        p = __slash(__s(p));
        if (over !== undefined && !__cond(over) && __nova.attr(p) >= 0) throw __error(58, 'File already exists');
        return __open(p, 2, true);
    },
    OpenTextFile: function (p, mode, create) {
        return __open(p, mode === undefined ? 1 : Number(mode), create === undefined ? false : __cond(create));
    }
};
var __fso = new __FileSystemObject();

/* ---------------------------------------------------------------------
 * Scripting.Dictionary
 * --------------------------------------------------------------------- */
function __Dictionary() { this.__keys = []; this.__vals = []; this.__mode = 0; }
__Dictionary.prototype = {
    __find: function (k) {
        for (var i = 0; i < this.__keys.length; i++) {
            var x = this.__keys[i];
            if (x === k || (this.__mode && typeof x === 'string' && typeof k === 'string' && x.toLowerCase() === k.toLowerCase()))
                return i;
            if (typeof x !== 'object' && typeof k !== 'object' && __s(x) === __s(k) && typeof x === typeof k) return i;
        }
        return -1;
    },
    Add: function (k, v) {
        if (this.__find(k) >= 0) throw __error(457, 'This key is already associated with an element of this collection');
        this.__keys.push(k); this.__vals.push(v);
    },
    Exists: function (k) { return this.__find(k) >= 0; },
    Item: function (k) { var i = this.__find(k); return i < 0 ? undefined : this.__vals[i]; },
    put_Item: function (k, v) {
        var i = this.__find(k);
        if (i < 0) { this.__keys.push(k); this.__vals.push(v); } else this.__vals[i] = v;
    },
    Keys: function () { return this.__keys.slice(); },
    Items: function () { return this.__vals.slice(); },
    Remove: function (k) {
        var i = this.__find(k);
        if (i < 0) throw __error(32811, 'Element not found');
        this.__keys.splice(i, 1); this.__vals.splice(i, 1);
    },
    RemoveAll: function () { this.__keys = []; this.__vals = []; }
};
__getter(__Dictionary.prototype, 'Count', function () { return this.__keys.length; });
__getter(__Dictionary.prototype, 'CompareMode', function () { return this.__mode; }, function (m) { this.__mode = Number(m); });

/* ---------------------------------------------------------------------
 * WScript.Shell
 * --------------------------------------------------------------------- */
/* "HKLM\Software\X\Name" as [root, key, value name]; a trailing \ names
 * the key's default value */
function __regpath(p) {
    p = __s(p);
    var i = p.indexOf('\\');
    var root = __roots[(i < 0 ? p : p.substr(0, i)).toLowerCase()];
    if (root === undefined) throw __error(-2147024894, 'Invalid root in registry key "' + p + '"');
    var rest = i < 0 ? '' : p.substr(i + 1), j = rest.lastIndexOf('\\');
    return [root, j < 0 ? '' : rest.substr(0, j), j < 0 ? rest : rest.substr(j + 1)];
}

function __Environment(kind) { this.__k = kind; }
__Environment.prototype = {
    Item: function (name) { return __nova.env_get(__s(name)); },
    put_Item: function (name, v) { __nova.env_set(__s(name), __s(v)); },
    Remove: function (name) { __nova.env_set(__s(name), null); }
};

function __WshShell() {}
__WshShell.prototype = {
    RegRead: function (p) {
        var r = __regpath(p), v = __nova.reg_read(r[0], r[1], r[2] === '' ? null : r[2]);
        if (v === undefined) throw __error(-2147024894, 'Unable to open registry key "' + p + '" for reading');
        return v;
    },
    RegWrite: function (p, v, type) {
        var r = __regpath(p);
        type = type === undefined ? (typeof v === 'number' ? 'REG_DWORD' : 'REG_SZ') : __s(type).toUpperCase();
        if (!__nova.reg_write(r[0], r[1], r[2] === '' ? null : r[2], type === 'REG_DWORD' ? Number(v) : __s(v), type))
            throw __error(-2147024891, 'Unable to save registry key "' + p + '"');
    },
    RegDelete: function (p) {
        var r = __regpath(p);
        if (!__nova.reg_delete(r[0], r[1], r[2] === '' ? null : r[2]))
            throw __error(-2147024894, 'Unable to remove registry key "' + p + '"');
    },
    ExpandEnvironmentStrings: function (s) { return __nova.env_expand(__s(s)); },
    Run: function (cmd, style, wait) {
        return __nova.run(__nova.env_expand(__s(cmd)), wait === undefined ? false : __cond(wait));
    },
    Environment: function (kind) { return new __Environment(kind === undefined ? 'SYSTEM' : __s(kind)); },
    SpecialFolders: function (name) {
        var n = __s(name).toLowerCase(), e = function (v) { return __nova.env_get(v); };
        var start = e('APPDATA') + '\\Microsoft\\Windows\\Start Menu';
        var all = e('ProgramData') + '\\Microsoft\\Windows\\Start Menu';
        var map = {
            desktop: e('USERPROFILE') + '\\Desktop', alluserdesktop: e('PUBLIC') + '\\Desktop',
            alluserstartmenu: all, alluserprograms: all + '\\Programs', allusersstartup: all + '\\Programs\\Startup',
            appdata: e('APPDATA'), mydocuments: e('USERPROFILE') + '\\Documents', startmenu: start,
            programs: start + '\\Programs', startup: start + '\\Programs\\Startup', templates: e('APPDATA') + '\\Microsoft\\Windows\\Templates'
        };
        return map[n] || '';
    },
    Popup: function (text) { __nova.log('Popup: ' + __s(text)); return -1; },
    LogEvent: function (type, text) { __nova.log('LogEvent: ' + __s(text)); return true; }
};
__getter(__WshShell.prototype, 'CurrentDirectory', function () { return __nova.full('.'); });

/* CreateObject (VBScript) and new ActiveXObject (JScript) */
function __create(progid) {
    switch (__s(progid).toLowerCase()) {
    case 'scripting.filesystemobject': return new __FileSystemObject();
    case 'scripting.dictionary': return new __Dictionary();
    case 'wscript.shell': return new __WshShell();
    case 'windowsinstaller.installer': return __installer;
    }
    __nova.log('CreateObject("' + __s(progid) + '"): no such object in NovaOS');
    throw __error(429, "ActiveX component can't create object: '" + __s(progid) + "'");
}
function ActiveXObject(progid) { return __create(progid); }
function GetObject() { throw __error(429, "ActiveX component can't create object"); }

/* ---------------------------------------------------------------------
 * Late binding for translated VBScript: members by any case, a method
 * named without arguments is called, a(i) indexes arrays and calls a
 * collection's default member (Item)
 * --------------------------------------------------------------------- */
function __key(o, name) {
    if (o === null || o === undefined || (typeof o !== 'object' && typeof o !== 'function'))
        throw __error(424, "Object required: '" + name + "'");
    if (name in o) return name;
    var lc = name.toLowerCase();
    for (var k in o) if (k.toLowerCase() === lc) return k;
    return null;
}
function __nomember(name) { return __error(438, "Object doesn't support this property or method: '" + name + "'"); }

/* o.name */
function __m(o, name) {
    var k = __key(o, name);
    if (k === null) throw __nomember(name);
    var v = o[k];
    return typeof v === 'function' ? v.call(o) : v;
}
/* o.name(args) */
function __mc(o, name, args) {
    var k = __key(o, name);
    if (k === null) throw __nomember(name);
    var v = o[k];
    if (typeof v === 'function') return v.apply(o, args);
    return args.length ? __idx(v, args) : v;
}
/* o.name = v */
function __mset(o, name, v) {
    var k = __key(o, 'put_' + name);
    if (k !== null) { o[k](v); return v; }
    k = __key(o, name);
    o[k === null ? name : k] = v;
    return v;
}
/* o.name(args) = v (JScript's compiled form passes the arguments after v) */
function __put(o, name, v) {
    var args = Array.prototype.slice.call(arguments, 3);
    var k = __key(o, 'put_' + name);
    if (k !== null) { o[k].apply(o, args.concat([v])); return v; }
    k = __key(o, name);
    if (k === null) throw __nomember(name);
    __idxset(typeof o[k] === 'function' ? o[k].call(o) : o[k], v, args);
    return v;
}
/* v(args) */
function __idx(v, args) {
    if (v instanceof Array) {
        for (var i = 0; i < args.length; i++) {
            var n = __int(args[i]);
            if (!(v instanceof Array) || n < 0 || n >= v.length) throw __error(9, 'Subscript out of range');
            v = v[n];
        }
        return v;
    }
    if (typeof v === 'function') return v.apply(null, args);
    if (v && typeof v === 'object') {
        if (__key(v, 'Item') !== null) return __mc(v, 'Item', args);
    }
    throw __error(13, 'Type mismatch');
}
function __idxset(v, val, args) {
    if (v instanceof Array) {
        for (var i = 0; i < args.length - 1; i++) v = __idx(v, [args[i]]);
        var n = __int(args[args.length - 1]);
        if (!(v instanceof Array) || n < 0 || n >= v.length) throw __error(9, 'Subscript out of range');
        v[n] = val;
        return val;
    }
    if (v && typeof v === 'object' && __key(v, 'put_Item') !== null) return __put.apply(null, [v, 'Item', val].concat(args));
    throw __error(13, 'Type mismatch');
}
