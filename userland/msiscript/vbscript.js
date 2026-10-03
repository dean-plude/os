/*
 * vbscript.js: VBScript to JavaScript, for script custom actions.
 *
 * __vbs_translate(source) returns JavaScript that mujs runs with
 * vbslib.js's helpers: every VBScript name becomes $ and its lower-case
 * spelling, operators become calls (__add, __cat, __eq...) that follow
 * VBScript's variant rules, and members are looked up by name at run time
 * (__m, __mc, __mset, __put) so any case finds them.  Each statement is
 * written on its own source line, so a run-time error's JavaScript line is
 * the script's line.
 *
 * Covered: Dim/ReDim/Const/Set, If (block and one-line), For/Next, For
 * Each, Do/Loop, While/Wend, Select Case, With, Sub and Function (with
 * Exit), Call, On Error Resume Next / GoTo 0 and Err, and the built-in
 * functions in vbslib.js.  Not covered: Class, Execute, ByRef arguments
 * (every argument is passed by value) and Option Compare.
 */
var __global = this;

function __vbs_translate(src) {
    var toks = [], pos = 0;

    function err(msg, tok) {
        var line = tok ? tok.line : (toks[pos] ? toks[pos].line : 0);
        var e = __error(1002, 'VBScript compilation error: ' + msg + ' (line ' + line + ')');
        e.line = line;
        return e;
    }

    /* ---------------- lexer ---------------- */
    (function lex() {
        var i = 0, n = src.length, line = 1, sp = false;
        function push(t, v) { toks.push({ t: t, v: v, line: line, sp: sp }); sp = false; }
        function startOfStatement() { return !toks.length || toks[toks.length - 1].t === 'nl'; }
        while (i < n) {
            var c = src.charAt(i), m;
            if (c === ' ' || c === '\t' || c === '\f' || c === '\v' || c === '\u00a0' || c === '\ufeff') { i++; sp = true; continue; }
            if (c === '\r' || c === '\n') {
                push('nl', '\n');
                if (c === '\r' && src.charAt(i + 1) === '\n') i++;
                i++; line++;
                continue;
            }
            if (c === "'") { while (i < n && src.charAt(i) !== '\r' && src.charAt(i) !== '\n') i++; continue; }
            if (c === '_') {
                var j = i + 1;
                while (j < n && (src.charAt(j) === ' ' || src.charAt(j) === '\t')) j++;
                if (j >= n || src.charAt(j) === '\r' || src.charAt(j) === '\n') {     /* line continuation */
                    if (src.charAt(j) === '\r' && src.charAt(j + 1) === '\n') j++;
                    i = j + 1; line++; sp = true;
                    continue;
                }
            }
            if (c === ':') { push('nl', ':'); i++; continue; }
            if (c === '"') {
                var s = '';
                i++;
                for (;;) {
                    if (i >= n || src.charAt(i) === '\r' || src.charAt(i) === '\n') throw err('Unterminated string constant', { line: line });
                    if (src.charAt(i) === '"') {
                        if (src.charAt(i + 1) === '"') { s += '"'; i += 2; continue; }
                        i++;
                        break;
                    }
                    s += src.charAt(i++);
                }
                push('str', s);
                continue;
            }
            var rest = src.substr(i, 64);
            if ((m = /^&H([0-9A-Fa-f]+)&?/.exec(rest))) { push('num', parseInt(m[1], 16) | 0); i += m[0].length; continue; }
            if ((m = /^&O?([0-7]+)&?/.exec(rest))) { push('num', parseInt(m[1], 8)); i += m[0].length; continue; }
            if ((m = /^(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?[%&!#@]?/.exec(rest))) {
                push('num', parseFloat(m[1] + (m[2] || ''))); i += m[0].length;
                continue;
            }
            if (c === '#') {
                var close = src.indexOf('#', i + 1);
                if (close < 0) throw err('Expected #', { line: line });
                push('date', src.substring(i + 1, close));
                i = close + 1;
                continue;
            }
            if ((m = /^[A-Za-z][A-Za-z0-9_]*/.exec(rest))) {
                var word = m[0];
                while (i + word.length < n && /[A-Za-z0-9_]/.test(src.charAt(i + word.length))) word = src.substr(i, word.length + 1);
                if (word.toLowerCase() === 'rem' && startOfStatement()) {
                    while (i < n && src.charAt(i) !== '\r' && src.charAt(i) !== '\n') i++;
                    continue;
                }
                push('id', word); i += word.length;
                continue;
            }
            if (c === '[') {
                var e2 = src.indexOf(']', i);
                if (e2 < 0) throw err("Expected ']'", { line: line });
                push('id', src.substring(i + 1, e2));
                i = e2 + 1;
                continue;
            }
            var two = src.substr(i, 2);
            if (two === '<>' || two === '<=' || two === '>=' || two === '=<' || two === '=>') {
                push('op', two === '=<' ? '<=' : two === '=>' ? '>=' : two); i += 2;
                continue;
            }
            if ('=<>+-*/\\^&(),.'.indexOf(c) >= 0) { push('op', c); i++; continue; }
            throw err("Invalid character '" + c + "'", { line: line });
        }
        push('nl', '\n');
        push('eof', '');
    })();

    /* ---------------- names and scopes ---------------- */
    function lc(name) { return name.toLowerCase().replace(/[^a-z0-9_]/g, function (ch) { return '_x' + ch.charCodeAt(0).toString(16); }); }
    function q(s) {
        return '"' + s.replace(/[\\"\u0000-\u001f\u2028\u2029]/g, function (ch) {
            if (ch === '\\') return '\\\\';
            if (ch === '"') return '\\"';
            return '\\u' + ('000' + ch.charCodeAt(0).toString(16)).substr(-4);
        }) + '"';
    }

    var userFuncs = {};                   /* every Sub and Function, found first: they may be called before they appear */
    for (var k = 0; k < toks.length - 1; k++) {
        var tk = toks[k], v = tk.t === 'id' ? tk.v.toLowerCase() : '';
        if ((v === 'sub' || v === 'function') && toks[k + 1].t === 'id') {
            var prev = k ? toks[k - 1] : null, pv = prev && prev.t === 'id' ? prev.v.toLowerCase() : '';
            if (!prev || prev.t === 'nl' || pv === 'public' || pv === 'private') userFuncs[lc(toks[k + 1].v)] = true;
        }
    }
    function isRuntimeName(l) { return l === 'session' || typeof __global['$' + l] !== 'undefined'; }
    function isFunc(l) { return userFuncs[l] === true || typeof __global['$' + l] === 'function'; }

    var globalScope = { names: {}, global: true, resume: false };
    var scope = globalScope, procs = [], tmp = 0;
    function use(l) { scope.names[l] = true; return '$' + l; }
    function ref(l) {
        if (scope.proc && l === scope.proc && scope.isFunction) return '$' + l + '$r';
        return use(l);
    }

    /* ---------------- output, a line per source line ---------------- */
    var out = [];
    function emit(line, code) { out[line - 1] = (out[line - 1] ? out[line - 1] + ' ' : '') + code; }
    function stmt(line, code) {
        emit(line, scope.resume ? 'try { ' + code + ' } catch (__e) { __catch(__e); }' : code);
    }

    /* ---------------- tokens ---------------- */
    function peek(o) { return toks[pos + (o || 0)]; }
    function next() { return toks[pos++]; }
    function isKw(tok, w) { return tok.t === 'id' && tok.v.toLowerCase() === w; }
    function isOp(tok, o) { return tok.t === 'op' && tok.v === o; }
    function kw(w) { if (isKw(peek(), w)) { pos++; return true; } return false; }
    function op(o) { if (isOp(peek(), o)) { pos++; return true; } return false; }
    function expectKw(w) { if (!kw(w)) throw err("Expected '" + w + "'"); }
    function expectOp(o) { if (!op(o)) throw err("Expected '" + o + "'"); }
    function ident() { var t = next(); if (t.t !== 'id') throw err('Expected identifier', t); return t.v; }
    var oneLineIf = 0;
    function atEnd() {
        var t = peek();
        return t.t === 'nl' || t.t === 'eof' || (oneLineIf && isKw(t, 'else'));
    }
    function endStatement() {
        if (!atEnd()) throw err('Expected end of statement');
    }
    function skipNl() { while (peek().t === 'nl') pos++; }

    /* ---------------- expressions ---------------- */
    function binary(sub, ops) {
        return function () {
            var a = sub();
            for (;;) {
                var t = peek(), f = null;
                for (var i = 0; i < ops.length; i++) {
                    var o = ops[i];
                    if ((o[0] === 'k' && isKw(t, o[1])) || (o[0] === 'o' && isOp(t, o[1]))) { f = o[2]; break; }
                }
                if (!f) return a;
                pos++;
                a = f + '(' + a + ', ' + sub() + ')';
            }
        };
    }
    function powExpr() {
        var a = postfix();
        while (op('^')) a = '__pow(' + a + ', ' + (isOp(peek(), '-') ? (pos++, '__neg(' + postfix() + ')') : postfix()) + ')';
        return a;
    }
    function unary() {
        if (op('-')) return '__neg(' + unary() + ')';
        if (op('+')) return unary();
        return powExpr();
    }
    var mulExpr = binary(unary, [['o', '*', '__mul'], ['o', '/', '__div']]);
    var idivExpr = binary(mulExpr, [['o', '\\', '__idiv']]);
    var modExpr = binary(idivExpr, [['k', 'mod', '__mod']]);
    var addExpr = binary(modExpr, [['o', '+', '__add'], ['o', '-', '__sub']]);
    var catExpr = binary(addExpr, [['o', '&', '__cat']]);
    var cmpExpr = binary(catExpr, [['o', '=', '__eq'], ['o', '<>', '__ne'], ['o', '<', '__lt'], ['o', '>', '__gt'],
                                   ['o', '<=', '__le'], ['o', '>=', '__ge'], ['k', 'is', '__is']]);
    function notExpr() { if (kw('not')) return '__not(' + notExpr() + ')'; return cmpExpr(); }
    var andExpr = binary(notExpr, [['k', 'and', '__and']]);
    var orExpr = binary(andExpr, [['k', 'or', '__or']]);
    var xorExpr = binary(orExpr, [['k', 'xor', '__xor']]);
    var eqvExpr = binary(xorExpr, [['k', 'eqv', '__eqv']]);
    var expr = binary(eqvExpr, [['k', 'imp', '__imp']]);

    function args() {                     /* after '(' : the arguments and ')' */
        var a = [];
        if (op(')')) return a;
        for (;;) {
            a.push(isOp(peek(), ',') || isOp(peek(), ')') ? 'undefined' : expr());
            if (op(')')) return a;
            expectOp(',');
        }
    }

    /* a name with its members and argument lists: x, x.y(1).z, .y in a With */
    function chain(stopAtSpacedParen) {
        var t = peek(), c = { links: [] };
        if (isOp(t, '.')) {
            if (!withs.length) throw err("Invalid use of '.' outside With");
            c.base = { code: withs[withs.length - 1] };
        } else if (t.t === 'id') {
            pos++;
            c.base = { name: lc(t.v), tok: t };
        } else throw err('Expected expression', t);
        for (;;) {
            t = peek();
            if (isOp(t, '.') && (!t.sp || !c.links.length && c.base.code)) {
                pos++;
                var m = next();
                if (m.t !== 'id') throw err('Expected identifier', m);
                c.links.push({ m: m.v });
            } else if (isOp(t, '(') && !(stopAtSpacedParen && t.sp)) {
                pos++;
                c.links.push({ args: args() });
            } else return c;
        }
    }

    /* JavaScript for a chain's value, from link @from */
    function rv(c, upto) {
        var links = c.links.slice(0, upto === undefined ? c.links.length : upto), code, i = 0;
        if (c.base.code) code = c.base.code;
        else {
            var l = c.base.name;
            var own = scope.proc === l && scope.isFunction && !(links[0] && links[0].args);
            if (isFunc(l) && !own) {
                if (links[0] && links[0].args) { code = '$' + l + '(' + links[0].args.join(', ') + ')'; i = 1; }
                else code = '$' + l + '()';
            } else code = ref(l);
        }
        for (; i < links.length; i++) {
            var k = links[i];
            if (k.m !== undefined) {
                if (links[i + 1] && links[i + 1].args) { code = '__mc(' + code + ', ' + q(k.m) + ', [' + links[i + 1].args.join(', ') + '])'; i++; }
                else code = '__m(' + code + ', ' + q(k.m) + ')';
            } else code = '__idx(' + code + ', [' + k.args.join(', ') + '])';
        }
        return code;
    }

    function postfix() {
        var t = peek();
        if (t.t === 'num') { pos++; return String(t.v); }
        if (t.t === 'str') { pos++; return q(t.v); }
        if (t.t === 'date') { pos++; return '__date(' + q(t.v) + ')'; }
        if (isOp(t, '(')) { pos++; var e = expr(); expectOp(')'); return '(' + e + ')'; }
        if (t.t === 'id') {
            switch (t.v.toLowerCase()) {
            case 'true': pos++; return 'true';
            case 'false': pos++; return 'false';
            case 'nothing': pos++; return 'null';
            case 'empty': pos++; return 'undefined';
            case 'null': pos++; return '__vbnull';
            case 'new': throw err('Classes are not supported', t);
            case 'me': throw err("'Me' is only valid in a Class", t);
            }
        }
        return rv(chain(false));
    }

    /* ---------------- statements ---------------- */
    function callStmt(line, c, extra) {
        var last = c.links[c.links.length - 1];
        if (extra === null && last && last.args) { stmt(line, rv(c) + ';'); return; }
        extra = extra || [];
        if (!last) {
            if (c.base.code) throw err('Expected statement');
            var l = c.base.name;
            stmt(line, (isFunc(l) ? '$' + l + '(' + extra.join(', ') + ')' : '__idx(' + ref(l) + ', [' + extra.join(', ') + '])') + ';');
        } else if (last.m !== undefined) {
            stmt(line, '__mc(' + rv(c, c.links.length - 1) + ', ' + q(last.m) + ', [' + extra.join(', ') + ']);');
        } else stmt(line, '__idx(' + rv(c) + ', [' + extra.join(', ') + ']);');
    }

    function assign(line, c, value) {
        var n = c.links.length, last = c.links[n - 1];
        if (!last) {
            if (c.base.code) throw err('Expected statement');
            stmt(line, ref(c.base.name) + ' = ' + value + ';');
        } else if (last.m !== undefined) {
            stmt(line, '__mset(' + rv(c, n - 1) + ', ' + q(last.m) + ', ' + value + ');');
        } else if (n >= 2 && c.links[n - 2].m !== undefined) {
            stmt(line, '__put(' + [rv(c, n - 2), q(c.links[n - 2].m), value].concat(last.args).join(', ') + ');');
        } else if (n === 1 && c.base.name) {
            stmt(line, '__idxset(' + ref(c.base.name) + ', ' + value + ', [' + last.args.join(', ') + ']);');
        } else stmt(line, '__idxset(' + rv(c, n - 1) + ', ' + value + ', [' + last.args.join(', ') + ']);');
    }

    function argList() {
        var a = [];
        for (;;) {
            a.push(isOp(peek(), ',') ? 'undefined' : expr());
            if (!op(',')) return a;
        }
    }

    /* an assignment or a call */
    function simple(line) {
        var c = chain(true);
        if (op('=')) { assign(line, c, expr()); return; }
        if (atEnd()) { callStmt(line, c, null); return; }
        if (isOp(peek(), '(')) {
            var save = pos;
            pos++;
            var a = args();
            if (atEnd()) { c.links.push({ args: a }); callStmt(line, c, null); return; }
            if (op('=')) { c.links.push({ args: a }); assign(line, c, expr()); return; }
            pos = save;
        }
        callStmt(line, c, argList());
    }

    function declare(name) { scope.names[name] = true; if (scope.proc) scope.declared[name] = true; }

    function dimList(line) {
        do {
            var name = lc(ident());
            declare(name);
            if (op('(')) {
                var dims = isOp(peek(), ')') ? [] : argList();
                expectOp(')');
                stmt(line, '$' + name + ' = ' + (dims.length ? '__dim(' + dims.join(', ') + ')' : '[]') + ';');
            }
        } while (op(','));
    }

    var withs = [], loops = [];

    /* statements until one of @ends ('end if', 'next', ...) */
    function block(ends) {
        for (;;) {
            skipNl();
            var t = peek();
            if (t.t === 'eof') {
                if (ends.length) throw err("Expected '" + ends[0] + "'", t);
                return;
            }
            var w = t.t === 'id' ? t.v.toLowerCase() : '';
            var w2 = w === 'end' || w === 'loop' ? w + ' ' + (peek(1).t === 'id' ? peek(1).v.toLowerCase() : '') : '';
            for (var i = 0; i < ends.length; i++) if (ends[i] === w || ends[i] === w2 || (ends[i] === 'loop' && w === 'loop')) return;
            statement();
        }
    }

    function procedure(line, isFunction) {
        if (scope.proc) throw err('Nested procedures are not allowed');
        var name = lc(ident()), params = [];
        if (op('(')) {
            if (!op(')')) {
                do {
                    if (!kw('byval')) kw('byref');
                    var p = lc(ident());
                    if (op('(')) expectOp(')');
                    params.push(p);
                } while (op(','));
                expectOp(')');
            }
        }
        endStatement();
        var outer = scope, id = procs.length;
        scope = { names: {}, declared: {}, params: params, proc: name, isFunction: isFunction, resume: false, id: id };
        procs.push(scope);
        emit(line, 'function $' + name + '(' + params.map(function (p) { return '$' + p; }).join(', ') + ') {' +
             (isFunction ? ' var $' + name + '$r;' : '') + ' \u0001' + id + '\u0001');
        block([isFunction ? 'end function' : 'end sub']);
        var e = next();
        next();
        emit(e.line, (isFunction ? 'return $' + name + '$r; ' : '') + '}');
        scope = outer;
    }

    function cond() { return '__cond(' + expr() + ')'; }

    function ifStatement(line) {
        var c = cond();
        expectKw('then');
        if (peek().t === 'nl' && peek().v === '\n' || peek().t === 'eof') {
            emit(line, 'if (' + c + ') {');
            for (;;) {
                block(['elseif', 'else', 'end if']);
                var t = next();
                var w = t.v.toLowerCase();
                if (w === 'elseif') {
                    var c2 = cond();
                    expectKw('then');
                    emit(t.line, '} else if (' + c2 + ') {');
                } else if (w === 'else') {
                    emit(t.line, '} else {');
                    block(['end if']);
                    var e = next();
                    next();
                    emit(e.line, '}');
                    return;
                } else {                                /* end if */
                    next();
                    emit(t.line, '}');
                    return;
                }
            }
        }
        /* one line: If c Then a: b Else c */
        emit(line, 'if (' + c + ') {');
        oneLineIf++;
        for (;;) {
            if (!(peek().t === 'nl' && peek().v === '\n') && !isKw(peek(), 'else')) statement(true);
            if (peek().t === 'nl' && peek().v === ':') { pos++; continue; }
            break;
        }
        if (kw('else')) {
            emit(line, '} else {');
            for (;;) {
                if (!(peek().t === 'nl' && peek().v === '\n')) statement(true);
                if (peek().t === 'nl' && peek().v === ':') { pos++; continue; }
                break;
            }
        }
        oneLineIf--;
        emit(line, '}');
    }

    function forStatement(line) {
        var n = ++tmp;
        if (kw('each')) {
            var v = lc(ident());
            expectKw('in');
            var coll = expr();
            emit(line, 'var __a' + n + ' = __foreach(' + coll + '); for (var __i' + n + ' = 0; __i' + n + ' < __a' + n +
                 '.length; __i' + n + '++) { ' + ref(v) + ' = __a' + n + '[__i' + n + '];');
        } else {
            var cv = ref(lc(ident()));
            expectOp('=');
            var from = expr();
            expectKw('to');
            var to = expr(), step = '1';
            if (kw('step')) step = expr();
            emit(line, 'var __e' + n + ', __s' + n + '; for (' + cv + ' = __num(' + from + '), __e' + n + ' = __num(' + to +
                 '), __s' + n + ' = __num(' + step + '); __s' + n + ' >= 0 ? ' + cv + ' <= __e' + n + ' : ' + cv + ' >= __e' + n +
                 '; ' + cv + ' += __s' + n + ') {');
        }
        loops.push('for');
        block(['next']);
        loops.pop();
        var t = next();
        if (peek().t === 'id') pos++;           /* Next i */
        emit(t.line, '}');
    }

    function doStatement(line) {
        var c = null;
        if (kw('while')) c = cond();
        else if (kw('until')) c = '!' + cond();
        emit(line, c ? 'while (' + c + ') {' : 'do {');
        loops.push('do');
        block(['loop']);
        loops.pop();
        var t = next();
        if (c) { emit(t.line, '}'); return; }
        if (kw('while')) emit(t.line, '} while (' + cond() + ');');
        else if (kw('until')) emit(t.line, '} while (!' + cond() + ');');
        else emit(t.line, '} while (true);');
    }

    function selectStatement(line) {
        expectKw('case');
        var n = ++tmp, v = '__c' + n, first = true;
        emit(line, 'var ' + v + ' = ' + expr() + ';');
        for (;;) {
            block(['case', 'end select']);
            var t = next();
            if (isKw(t, 'end')) { next(); if (!first) emit(t.line, '}'); return; }
            if (kw('else')) { emit(t.line, first ? 'if (true) {' : '} else {'); first = false; continue; }
            var tests = [];
            do {
                if (kw('is')) {
                    var o = next(), f = { '=': '__eq', '<>': '__ne', '<': '__lt', '>': '__gt', '<=': '__le', '>=': '__ge' }[o.v];
                    if (!f) throw err('Expected comparison', o);
                    tests.push('__cond(' + f + '(' + v + ', ' + expr() + '))');
                } else {
                    var a = expr();
                    if (kw('to')) tests.push('(__cmp(' + v + ', ' + a + ') >= 0 && __cmp(' + v + ', ' + expr() + ') <= 0)');
                    else tests.push('__cmp(' + v + ', ' + a + ') === 0');
                }
            } while (op(','));
            emit(t.line, (first ? 'if (' : '} else if (') + tests.join(' || ') + ') {');
            first = false;
        }
    }

    function statement(inline) {
        var t = peek(), line = t.line;
        var w = t.t === 'id' ? t.v.toLowerCase() : '';
        if ((w === 'public' || w === 'private') && peek(1).t === 'id') {
            var w1 = peek(1).v.toLowerCase();
            pos++;
            if (w1 === 'sub' || w1 === 'function' || w1 === 'const') { w = w1; t = peek(); }
            else { dimList(line); endStatement(); return; }
        }
        switch (w) {
        case 'dim': pos++; dimList(line); break;
        case 'redim':
            pos++;
            var keep = kw('preserve');
            do {
                var name = lc(ident());
                declare(name);
                expectOp('(');
                var dims = argList();
                expectOp(')');
                stmt(line, '$' + name + ' = ' + (keep ? '__redim($' + name + ', ' : '__dim(') + dims.join(', ') + ');');
            } while (op(','));
            break;
        case 'const':
            pos++;
            do {
                var cn = lc(ident());
                declare(cn);
                expectOp('=');
                stmt(line, '$' + cn + ' = ' + expr() + ';');
            } while (op(','));
            break;
        case 'erase': pos++; var en = lc(ident()); stmt(line, ref(en) + ' = __dim($ubound(' + ref(en) + '));'); break;
        case 'set': case 'let': {
            pos++;
            var c = chain(false);
            expectOp('=');
            assign(line, c, expr());
            break;
        }
        case 'call': {
            pos++;
            var cc = chain(false);
            callStmt(line, cc, null);
            break;
        }
        case 'if': pos++; ifStatement(line); return;
        case 'for': pos++; forStatement(line); return;
        case 'do': pos++; doStatement(line); return;
        case 'while':
            pos++;
            emit(line, 'while (' + cond() + ') {');
            loops.push('do');
            block(['wend']);
            loops.pop();
            emit(next().line, '}');
            return;
        case 'select': pos++; selectStatement(line); return;
        case 'with': {
            pos++;
            var wv = '__w' + (++tmp);
            emit(line, 'var ' + wv + ' = ' + expr() + ';');
            withs.push(wv);
            block(['end with']);
            withs.pop();
            next(); next();
            break;
        }
        case 'sub': case 'function':
            if (inline) throw err('Expected statement', t);
            pos++;
            procedure(line, w === 'function');
            return;
        case 'exit': {
            pos++;
            var x = next(), xw = x.t === 'id' ? x.v.toLowerCase() : '';
            if (xw === 'for' || xw === 'do') stmt(line, 'break;');
            else if (xw === 'sub' || xw === 'property') emit(line, 'return;');
            else if (xw === 'function') emit(line, 'return $' + scope.proc + '$r;');
            else throw err("Expected 'For', 'Do', 'Sub' or 'Function'", x);
            break;
        }
        case 'on': {
            pos++;
            expectKw('error');
            if (kw('resume')) { expectKw('next'); scope.resume = true; }
            else { expectKw('goto'); var z = next(); if (z.t !== 'num' || z.v !== 0) throw err('Only On Error GoTo 0 is supported', z); scope.resume = false; }
            emit(line, '$err.Clear();');
            break;
        }
        case 'option': pos++; while (!atEnd()) pos++; break;
        case 'randomize': pos++; if (!atEnd()) expr(); break;
        case 'stop': pos++; break;
        case 'class': throw err('Class statements are not supported', t);
        case 'execute': case 'executeglobal': throw err('Execute is not supported', t);
        default: simple(line);
        }
        if (!inline) endStatement();
    }

    block([]);

    /* declarations: the global names, then each procedure's locals */
    var globals = {}, decl = [];
    for (var g in globalScope.names) if (!isRuntimeName(g) && !userFuncs[g]) { globals[g] = true; decl.push('$' + g); }
    var js = out.join('\n');
    for (var p = 0; p < procs.length; p++) {
        var sc = procs[p], locals = [];
        for (var nm in sc.names) {
            if (sc.params.indexOf(nm) >= 0 || userFuncs[nm] || isRuntimeName(nm)) continue;
            if (globals[nm] && !sc.declared[nm]) continue;
            locals.push('$' + nm);
        }
        js = js.replace('\u0001' + p + '\u0001', locals.length ? 'var ' + locals.join(', ') + ';' : '');
    }
    if (decl.length) js += ' var ' + decl.join(', ') + ';';
    return js;
}
