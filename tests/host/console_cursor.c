#include "../../kernel/um/um_console.c"
#include <stddef.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
extern int vsnprintf(char *, size_t, const char *, __builtin_va_list);
extern int puts(const char *);
static int stopping, waits;
static UmConsole *drain;
static UmProcess fake;
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
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
    c.screen[0].used=true; c.screen[0].cols=80; c.screen[0].rows=25; c.screen[0].attr=7;
    c.screen[0].cells=kmalloc(80*25*sizeof(*c.screen[0].cells)); assert(c.screen[0].cells);
    for (int i=0;i<80*25;i++) c.screen[0].cells[i]=(UmConsoleCell){' ',7};
    UmConsoleScreenInfo info;
    assert(!um_console_screen_call(&c,0,CON_SCREEN_INFO,&info,sizeof(info),&r));
    assert(info.cols==80 && info.rows==25 && info.cursor_x==79 && info.cursor_y==24);
    struct { UmConsoleScreenRequest req; UINT16 out[4]; } linear={0};
    linear.req.x=2; linear.req.y=3; linear.req.count=3; linear.req.value='Q';
    assert(!um_console_screen_call(&c,0,CON_SCREEN_FILL_CHAR,&linear.req,sizeof(linear.req),&r) && r==3);
    linear.req.value=0x1E;
    assert(!um_console_screen_call(&c,0,CON_SCREEN_FILL_ATTR,&linear.req,sizeof(linear.req),&r) && r==3);
    linear.req.value=0;
    assert(!um_console_screen_call(&c,0,CON_SCREEN_READ_TEXT,&linear,sizeof(linear),&r) && r==3);
    assert(linear.out[0]=='Q' && linear.out[1]=='Q' && linear.out[2]=='Q');
    assert(!um_console_screen_call(&c,0,CON_SCREEN_READ_ATTR,&linear,sizeof(linear),&r) && r==3);
    assert(linear.out[0]==0x1E && linear.out[2]==0x1E);
    UmConsoleScreenRequest pos={.x=4,.y=5};
    assert(!um_console_screen_call(&c,0,CON_SCREEN_SET_CURSOR,&pos,sizeof(pos),&r));
    assert(!um_console_screen_call(&c,0,CON_SCREEN_INFO,&info,sizeof(info),&r));
    assert(info.cursor_x==4 && info.cursor_y==5);
    struct { UmConsoleScreenRequest req; UmConsoleCell cells[4]; } cells={0};
    cells.req.width=2; cells.req.height=2; cells.req.left=1; cells.req.top=1;
    cells.req.right=2; cells.req.bottom=2; cells.req.x=0; cells.req.y=0;
    cells.cells[0]=(UmConsoleCell){'A',0x1F}; cells.cells[1]=(UmConsoleCell){'B',0x2E};
    cells.cells[2]=(UmConsoleCell){'C',0x3D}; cells.cells[3]=(UmConsoleCell){'D',0x4C};
    assert(!um_console_screen_call(&c,0,CON_SCREEN_WRITE_CELLS,&cells,sizeof(cells),&r));
    assert(!um_console_screen_call(&c,0,CON_SCREEN_READ_CELLS,&cells,sizeof(cells),&r));
    assert(cells.cells[0].ch=='A' && cells.cells[3].ch=='D' && cells.cells[1].attr==0x2E);
    UmConsoleScreenRequest scroll={.left=1,.top=1,.right=2,.bottom=2,.dest_x=2,.dest_y=2,
        .value='.',.attributes=7};
    assert(!um_console_screen_call(&c,0,CON_SCREEN_SCROLL,&scroll,sizeof(scroll),&r));
    assert(c.screen[0].cells[1*80+1].ch=='.' && c.screen[0].cells[2*80+2].ch=='A');
    UINT32 second;
    assert(!um_console_screen_create(&c,&second) && second==1);
    UmConsoleScreenRequest resize={.width=2,.height=2};
    assert(!um_console_screen_call(&c,second,CON_SCREEN_SET_SIZE,&resize,sizeof(resize),&r));
    struct { UmConsoleScreenRequest req; UmConsoleCell cells[4]; } other={0};
    other.req.width=2; other.req.height=2; other.req.left=0; other.req.top=0;
    other.req.right=1; other.req.bottom=1;
    for (int i=0;i<4;i++) other.cells[i]=(UmConsoleCell){'X'+i,0x1F};
    assert(!um_console_screen_call(&c,second,CON_SCREEN_WRITE_CELLS,&other,sizeof(other),&r));
    assert(!um_console_screen_call(&c,second,CON_SCREEN_ACTIVATE,NULL,0,&r) && c.active_screen==second);
    assert(c.screen[1].cells[0].ch=='X' && c.screen[0].cells[0].ch==' ');
    um_console_screen_unref(&c, second);
    assert(c.screen[1].used);
    assert(!um_console_screen_call(&c,0,CON_SCREEN_ACTIVATE,NULL,0,&r) && !c.screen[1].used);
    free(c.screen[0].cells);
    puts("console queue and screen-buffer APIs: cursor, cells, fills, scroll, independent buffers, cleanup and backpressure passed");
}
