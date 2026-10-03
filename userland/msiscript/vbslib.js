/*
 * vbslib.js: VBScript's values, operators and built-in functions, for the
 * JavaScript vbscript.js translates a VBScript custom action into.
 *
 * Values: Empty is undefined, Nothing is null, Null is __vbnull, True and
 * False are JavaScript's booleans (numerically -1 and 0), numbers and
 * strings are themselves and arrays are JavaScript arrays (one inside the
 * other for more dimensions).  Every name the translation uses is a
 * lower-case name with a $ in front ($left, $vbcrlf, $session), so
 * VBScript's case-insensitive names cannot meet JavaScript's.
 */

var __vbnull = { toString: function () { return ''; } };

function __mismatch(v) { return __error(13, "Type mismatch: '" + __s(v) + "'"); }

/* a variant as a number (arithmetic, comparisons with numbers) */
function __num(v) {
    if (typeof v === 'number') return v;
    if (v === undefined || v === false) return 0;
    if (v === true) return -1;
    if (typeof v === 'string') {
        var s = v.replace(/^\s+|\s+$/g, ''), m;
        if ((m = /^&H([0-9A-F]+)$/i.exec(s))) return parseInt(m[1], 16);
        if ((m = /^&O([0-7]+)$/i.exec(s))) return parseInt(m[1], 8);
        if (s !== '' && !isNaN(Number(s))) return Number(s);
    }
    throw __mismatch(v);
}
function __isnum(v) {
    if (typeof v === 'number' || typeof v === 'boolean' || v === undefined) return true;
    if (typeof v !== 'string') return false;
    try { __num(v); return true; } catch (e) { return false; }
}
/* round half to even, as VBScript's integer conversions do */
function __round(x) {
    var f = Math.floor(x), d = x - f;
    if (d > 0.5 || (d === 0.5 && f % 2 !== 0)) return f + 1;
    return f;
}
function __int(v) { return __round(__num(v)); }

/* the truth of a condition */
function __cond(v) {
    if (v === true || v === false) return v;
    if (v === undefined) return false;
    if (v === __vbnull) return false;
    if (typeof v === 'string') {
        var l = v.toLowerCase();
        if (l === 'true') return true;
        if (l === 'false') return false;
    }
    return __num(v) !== 0;
}

function __add(a, b) {
    if (a === __vbnull || b === __vbnull) return __vbnull;
    if ((typeof a === 'string' || a === undefined) && (typeof b === 'string' || b === undefined) && !(a === undefined && b === undefined))
        return __s(a) + __s(b);
    return __num(a) + __num(b);
}
function __sub(a, b) { return __num(a) - __num(b); }
function __mul(a, b) { return __num(a) * __num(b); }
function __div(a, b) { var d = __num(b); if (d === 0) throw __error(11, 'Division by zero'); return __num(a) / d; }
function __idiv(a, b) {
    var d = __int(b);
    if (d === 0) throw __error(11, 'Division by zero');
    var q = __int(a) / d;
    return q < 0 ? Math.ceil(q) : Math.floor(q);
}
function __mod(a, b) { var d = __int(b); if (d === 0) throw __error(11, 'Division by zero'); return __int(a) % d; }
function __pow(a, b) { return Math.pow(__num(a), __num(b)); }
function __neg(a) { return -__num(a); }
function __cat(a, b) { return __s(a) + __s(b); }

/* -1, 0 or 1: numbers as numbers, strings as strings (binary compare), a
 * number and a numeric string as numbers */
function __cmp(a, b) {
    var x, y;
    if (typeof a === 'string' && typeof b === 'string') { x = a; y = b; }
    else if ((typeof a === 'string' && !__isnum(a)) || (typeof b === 'string' && !__isnum(b))) { x = __s(a); y = __s(b); }
    else if (typeof a === 'object' && a !== null && a !== __vbnull || typeof b === 'object' && b !== null && b !== __vbnull) {
        return a === b ? 0 : 1;
    } else { x = __num(a); y = __num(b); }
    return x < y ? -1 : x > y ? 1 : 0;
}
function __eq(a, b) { return a === __vbnull || b === __vbnull ? __vbnull : __cmp(a, b) === 0; }
function __ne(a, b) { return a === __vbnull || b === __vbnull ? __vbnull : __cmp(a, b) !== 0; }
function __lt(a, b) { return __cmp(a, b) < 0; }
function __gt(a, b) { return __cmp(a, b) > 0; }
function __le(a, b) { return __cmp(a, b) <= 0; }
function __ge(a, b) { return __cmp(a, b) >= 0; }
function __is(a, b) { return a === b || (a == null && b == null); }

/* the logical operators are bitwise on numbers, logical on booleans */
function __bool(v) { return v === true || v === false; }
function __not(a) { return __bool(a) ? !a : ~__int(a); }
function __and(a, b) { return __bool(a) && __bool(b) ? a && b : __int(a) & __int(b); }
function __or(a, b) { return __bool(a) && __bool(b) ? a || b : __int(a) | __int(b); }
function __xor(a, b) { return __bool(a) && __bool(b) ? a !== b : __int(a) ^ __int(b); }
function __eqv(a, b) { return __bool(a) && __bool(b) ? a === b : ~(__int(a) ^ __int(b)); }
function __imp(a, b) { return __bool(a) && __bool(b) ? !a || b : ~__int(a) | __int(b); }

/* Dim a(2, 3): arrays of Empty, nested for each further dimension */
function __dim() {
    var dims = arguments;
    function make(d) {
        var n = __int(dims[d]) + 1, a = new Array(n > 0 ? n : 0);
        if (d + 1 < dims.length) for (var i = 0; i < a.length; i++) a[i] = make(d + 1);
        return a;
    }
    return make(0);
}
function __redim(old) {
    var dims = Array.prototype.slice.call(arguments, 1), a = __dim.apply(null, dims);
    if (old instanceof Array) for (var i = 0; i < old.length && i < a.length; i++) a[i] = old[i];
    return a;
}

/* Err, and the errors On Error Resume Next catches */
function __ErrObject() { this.Number = 0; this.Description = ''; this.Source = ''; }
__ErrObject.prototype = {
    Clear: function () { this.Number = 0; this.Description = ''; this.Source = ''; },
    Raise: function (n, src, desc) {
        var e = __error(__num(n), desc === undefined ? 'Unknown runtime error' : __s(desc));
        e.source = src === undefined ? '' : __s(src);
        throw e;
    }
};
var $err = new __ErrObject();
function __catch(e) {
    $err.Number = typeof e.number === 'number' ? e.number : 5;
    $err.Description = e.message !== undefined ? String(e.message) : String(e);
    $err.Source = e.source || 'Microsoft VBScript runtime error';
}

/* the For Each loop's array of values */
var __foreach = __each;

/* ---------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------- */
var $vbcrlf = '\r\n', $vbnewline = '\r\n', $vbcr = '\r', $vblf = '\n', $vbtab = '\t', $vbformfeed = '\f';
var $vbverticaltab = '\v', $vbnullstring = '', $vbnullchar = '\0', $vbback = '\b';
var $vbtrue = -1, $vbfalse = 0, $vbusedefault = -2, $vbbinarycompare = 0, $vbtextcompare = 1;
var $vbokonly = 0, $vbokcancel = 1, $vbabortretryignore = 2, $vbyesnocancel = 3, $vbyesno = 4, $vbretrycancel = 5;
var $vbcritical = 16, $vbquestion = 32, $vbexclamation = 48, $vbinformation = 64;
var $vbdefaultbutton1 = 0, $vbdefaultbutton2 = 256, $vbdefaultbutton3 = 512, $vbapplicationmodal = 0, $vbsystemmodal = 4096;
var $vbok = 1, $vbcancel = 2, $vbabort = 3, $vbretry = 4, $vbignore = 5, $vbyes = 6, $vbno = 7;
var $vbempty = 0, $vbnull = 1, $vbinteger = 2, $vblong = 3, $vbsingle = 4, $vbdouble = 5, $vbcurrency = 6;
var $vbdate = 7, $vbstring = 8, $vbobject = 9, $vberror = 10, $vbboolean = 11, $vbvariant = 12, $vbbyte = 17, $vbarray = 8192;
var $vbobjecterror = -2147221504;
var $vbsunday = 1, $vbmonday = 2, $vbtuesday = 3, $vbwednesday = 4, $vbthursday = 5, $vbfriday = 6, $vbsaturday = 7;

/* ---------------------------------------------------------------------
 * Functions
 * --------------------------------------------------------------------- */
function __str(v) {
    if (typeof v === 'number') {
        var s = String(v);
        return s.indexOf('e') >= 0 ? s.replace('e', 'E') : s;
    }
    if (v === __vbnull) throw __error(94, 'Invalid use of Null');
    return __s(v);
}
function __len(n) { var x = __num(n); if (x < 0) throw __error(5, 'Invalid procedure call or argument'); return x; }

function $len(s) { return s === __vbnull ? __vbnull : __str(s).length; }
function $left(s, n) { return s === __vbnull ? __vbnull : __str(s).substr(0, __len(n)); }
function $right(s, n) { s = __str(s); n = __len(n); return n >= s.length ? s : s.substr(s.length - n); }
function $mid(s, start, n) {
    if (s === __vbnull) return __vbnull;
    s = __str(s); start = __num(start);
    if (start < 1) throw __error(5, 'Invalid procedure call or argument');
    return n === undefined ? s.substr(start - 1) : s.substr(start - 1, __len(n));
}
function $instr(a, b, c, mode) {
    var start = 1;
    if (c !== undefined) { start = __num(a); a = b; b = c; }
    if (a === __vbnull || b === __vbnull) return __vbnull;
    a = __str(a); b = __str(b);
    if (mode !== undefined && __num(mode) === 1) { a = a.toLowerCase(); b = b.toLowerCase(); }
    if (start > a.length) return 0;
    return a.indexOf(b, start - 1) + 1;
}
function $instrrev(a, b, start, mode) {
    a = __str(a); b = __str(b);
    if (mode !== undefined && __num(mode) === 1) { a = a.toLowerCase(); b = b.toLowerCase(); }
    var from = start === undefined || __num(start) === -1 ? a.length : __num(start) - b.length;
    if (from < 0) return 0;
    return a.lastIndexOf(b, from) + 1;
}
function $replace(s, find, repl, start, count, mode) {
    s = __str(s); find = __str(find); repl = __str(repl);
    start = start === undefined ? 1 : __num(start);
    count = count === undefined ? -1 : __num(count);
    var text = mode !== undefined && __num(mode) === 1;
    var src = s.substr(start - 1), out = '', i = 0;
    if (find === '') return src;
    var hay = text ? src.toLowerCase() : src, needle = text ? find.toLowerCase() : find;
    while (count !== 0) {
        var j = hay.indexOf(needle, i);
        if (j < 0) break;
        out += src.substring(i, j) + repl;
        i = j + find.length;
        count--;
    }
    return out + src.substr(i);
}
function $split(s, d, count, mode) {
    s = __str(s);
    d = d === undefined ? ' ' : __str(d);
    if (s === '') return [];
    if (d === '') return [s];
    var parts = s.split(d);
    count = count === undefined ? -1 : __num(count);
    if (count > 0 && parts.length > count) parts = parts.slice(0, count - 1).concat([parts.slice(count - 1).join(d)]);
    return parts;
}
function $join(a, d) {
    d = d === undefined ? ' ' : __str(d);
    var out = [];
    for (var i = 0; i < a.length; i++) out.push(__s(a[i]));
    return out.join(d);
}
function $ucase(s) { return s === __vbnull ? s : __str(s).toUpperCase(); }
function $lcase(s) { return s === __vbnull ? s : __str(s).toLowerCase(); }
function $trim(s) { return __str(s).replace(/^ +| +$/g, ''); }
function $ltrim(s) { return __str(s).replace(/^ +/, ''); }
function $rtrim(s) { return __str(s).replace(/ +$/, ''); }
function $space(n) { var s = ''; for (var i = __len(n); i > 0; i--) s += ' '; return s; }
function $string(n, c) {
    c = typeof c === 'number' ? String.fromCharCode(c) : __str(c).charAt(0);
    var s = '';
    for (var i = __len(n); i > 0; i--) s += c;
    return s;
}
function $strreverse(s) { return __str(s).split('').reverse().join(''); }
function $strcomp(a, b, mode) {
    a = __str(a); b = __str(b);
    if (mode !== undefined && __num(mode) === 1) { a = a.toLowerCase(); b = b.toLowerCase(); }
    return a < b ? -1 : a > b ? 1 : 0;
}
function $asc(s) { s = __str(s); if (s === '') throw __error(5, 'Invalid procedure call or argument'); return s.charCodeAt(0); }
var $ascw = $asc, $ascb = $asc;
function $chr(n) { return String.fromCharCode(__int(n)); }
var $chrw = $chr, $chrb = $chr;
function $cstr(v) { return __str(v); }
function $cint(v) { return __int(v); }
var $clng = $cint, $cbyte = $cint;
function $cdbl(v) { return __num(v); }
var $csng = $cdbl, $ccur = $cdbl;
function $cbool(v) { return __cond(v); }
function $cdate(v) { return v; }
function $int(v) { return Math.floor(__num(v)); }
function $fix(v) { var x = __num(v); return x < 0 ? Math.ceil(x) : Math.floor(x); }
function $round(v, d) {
    var p = Math.pow(10, d === undefined ? 0 : __num(d));
    return __round(__num(v) * p) / p;
}
function $abs(v) { return Math.abs(__num(v)); }
function $sgn(v) { var x = __num(v); return x > 0 ? 1 : x < 0 ? -1 : 0; }
function $sqr(v) { return Math.sqrt(__num(v)); }
function $exp(v) { return Math.exp(__num(v)); }
function $log(v) { return Math.log(__num(v)); }
function $sin(v) { return Math.sin(__num(v)); }
function $cos(v) { return Math.cos(__num(v)); }
function $tan(v) { return Math.tan(__num(v)); }
function $atn(v) { return Math.atan(__num(v)); }
function $rnd() { return Math.random(); }
function $hex(v) { var n = __int(v); return (n < 0 ? n + 4294967296 : n).toString(16).toUpperCase(); }
function $oct(v) { var n = __int(v); return (n < 0 ? n + 4294967296 : n).toString(8); }
function $isempty(v) { return v === undefined; }
function $isnull(v) { return v === __vbnull; }
function $isnumeric(v) { return v !== undefined && typeof v !== 'boolean' ? __isnum(v) : typeof v === 'boolean' || v === undefined; }
function $isobject(v) { return v === null || (typeof v === 'object' && v !== __vbnull && !(v instanceof Array)); }
function $isarray(v) { return v instanceof Array; }
function $isdate(v) { return v instanceof Date; }
function $array() { return Array.prototype.slice.call(arguments); }
function $ubound(a, d) {
    if (!(a instanceof Array)) throw __mismatch(a);
    for (var i = 1; i < (d === undefined ? 1 : __num(d)); i++) a = a[0];
    return a.length - 1;
}
function $lbound(a) { if (!(a instanceof Array)) throw __mismatch(a); return 0; }
function $vartype(v) {
    if (v === undefined) return 0;
    if (v === __vbnull) return 1;
    if (typeof v === 'number') return v === Math.floor(v) && Math.abs(v) < 2147483648 ? (Math.abs(v) < 32768 ? 2 : 3) : 5;
    if (typeof v === 'string') return 8;
    if (typeof v === 'boolean') return 11;
    if (v instanceof Array) return 8204;
    if (v instanceof Date) return 7;
    return 9;
}
function $typename(v) {
    if (v === null) return 'Nothing';
    if (v instanceof Array) return 'Variant()';
    var t = $vartype(v);
    if (t === 9) {
        var names = [[__Session, 'Session'], [__Installer, 'Installer'], [__Database, 'Database'], [__View, 'View'],
                     [__Record, 'Record'], [__FileSystemObject, 'FileSystemObject'], [__Dictionary, 'Dictionary'],
                     [__WshShell, 'IWshShell3'], [__File, 'File'], [__Folder, 'Folder'], [__TextStream, 'TextStream']];
        for (var i = 0; i < names.length; i++) if (v instanceof names[i][0]) return names[i][1];
        return 'Object';
    }
    return { 0: 'Empty', 1: 'Null', 2: 'Integer', 3: 'Long', 5: 'Double', 7: 'Date', 8: 'String', 11: 'Boolean' }[t];
}
function $now() { return new Date(); }
function $date() { var d = new Date(); return new Date(d.getFullYear(), d.getMonth(), d.getDate()); }
function $time() { return new Date(); }
function $timer() { var d = new Date(); return d.getHours() * 3600 + d.getMinutes() * 60 + d.getSeconds() + d.getMilliseconds() / 1000; }
function __date(v) { return v instanceof Date ? v : new Date(__s(v)); }
function $year(d) { return __date(d).getFullYear(); }
function $month(d) { return __date(d).getMonth() + 1; }
function $day(d) { return __date(d).getDate(); }
function $hour(d) { return __date(d).getHours(); }
function $minute(d) { return __date(d).getMinutes(); }
function $second(d) { return __date(d).getSeconds(); }
function $weekday(d) { return __date(d).getDay() + 1; }
function $createobject(progid) { return __create(progid); }
function $getobject() { return GetObject(); }
function $msgbox(text, buttons, title) {
    __nova.log('MsgBox: ' + __s(text));
    var b = buttons === undefined ? 0 : __int(buttons) & 7;
    return b === 4 || b === 3 ? 6 : b === 2 ? 3 : b === 5 ? 4 : 1;
}
function $inputbox(prompt, title, def) { __nova.log('InputBox: ' + __s(prompt)); return def === undefined ? '' : __s(def); }
function $filter(a, s, include, mode) {
    var out = [], text = mode !== undefined && __num(mode) === 1, keep = include === undefined ? true : __cond(include);
    s = __str(s);
    for (var i = 0; i < a.length; i++) {
        var x = __s(a[i]);
        if ((text ? x.toLowerCase().indexOf(s.toLowerCase()) : x.indexOf(s)) >= 0 === keep) out.push(a[i]);
    }
    return out;
}
function $scriptengine() { return 'VBScript'; }
