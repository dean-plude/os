#include "../../kernel/um/um_console.c"
#include <stddef.h>
#include <assert.h>
#include <stdarg.h>
extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
extern int puts(const char *);
static int stopping, waits;
static UmConsole *drain;
static UmProcess fake;
UmProcess *UmCurrent(void) { return &fake; }
bool um_stopping(void) { return stopping; }
void um_lock(UmLock *l) { (void)l; }
void um_unlock(UmLock *l) { (void)l; }
void sched_wait(void) { waits++; drain->out_tail = drain->out_head; }
int ksnprintf(char *b, size_t n, const char *f, ...) {
    va_list v; va_start(v,f); int k=vsnprintf(b,n,f,v); va_end(v); return k;
}
int main(void) {
    static UmConsole c;
    c.cols=80; c.rows=25; c.cursor_style=25|0x100; drain=&c;
    UINT32 r=0;
    assert(um_console_cursor(NULL,9,0,&r)==0xC0000008u);
    assert(!um_console_cursor(&c,9,79|(24<<16),&r));
    char b[64]; int n=UmConsoleRead(&c,b,sizeof(b)); b[n]=0;
    assert(!strcmp(b,"\x1b[25;80H"));
    assert(um_console_cursor(&c,9,80,&r)==0xC000000Du);
    assert(um_console_cursor(&c,9,25<<16,&r)==0xC000000Du);
    assert(um_console_cursor(&c,9,0xFFFF,&r)==0xC000000Du);
    assert(!um_console_cursor(&c,11,100,&r));
    n=UmConsoleRead(&c,b,sizeof(b)); b[n]=0; assert(!strcmp(b,"\x1b[?25l"));
    assert(!um_console_cursor(&c,10,0,&r) && r==100);
    assert(um_console_cursor(&c,11,0,&r)==0xC000000Du);
    assert(um_console_cursor(&c,11,101,&r)==0xC000000Du);
    assert(um_console_cursor(&c,11,0x201,&r)==0xC000000Du);
    assert(UmConsoleCursorStyle(&c)==100);
    c.out_head=OUT_SIZE; c.out_tail=0; stopping=1;
    assert(um_console_cursor(&c,11,1|0x100,&r)==0xC0000120u);
    assert(c.out_head==OUT_SIZE && UmConsoleCursorStyle(&c)==100);
    stopping=0;
    assert(!um_console_cursor(&c,11,1|0x100,&r) && waits==1);
    n=UmConsoleRead(&c,b,sizeof(b)); b[n]=0; assert(!strcmp(b,"\x1b[?25h"));
    assert(UmConsoleCursorStyle(&c)==(1|0x100));
    puts("actual console queue: cursor sequences, bounds, state, cancellation and backpressure passed");
}
