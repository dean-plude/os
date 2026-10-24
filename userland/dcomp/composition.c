/*
 * Windows.UI.Composition — the system compositor's Windows Runtime API, in
 * dcomp.dll as on Windows (its DllGetActivationFactory serves
 * Windows.UI.Composition.Compositor; the registry's ActivatableClassId key,
 * kernel/um/um_registry.c, names this DLL).  This is what the XAML
 * framework (Windows.UI.Xaml) draws with, and what WinRT desktop programs
 * use through ICompositorDesktopInterop.
 *
 * What is here: the Compositor; container and sprite visuals with their
 * offset, size, anchor and centre points, scale, rotation (about the Z
 * axis), 2-D transform matrix, opacity, visibility and inset clip, and a
 * visual collection (InsertAtTop/AtBottom/Above/Below, Remove, RemoveAll);
 * colour brushes and surface brushes (stretch None, Fill, Uniform and
 * UniformToFill with alignment ratios); targets on a window (the desktop
 * target, ICompositorDesktopInterop) or on the thread's CoreWindow
 * (CreateTargetForCurrentView); graphics devices
 * (ICompositorInterop::CreateGraphicsDevice, on a Direct3D 11 device or
 * an ID2D1Device) and their drawing surfaces, drawn through
 * ICompositionDrawingSurfaceInterop's BeginDraw (the surface's
 * ID3D11Texture2D or IDXGISurface, or an ID2D1DeviceContext on a
 * Direct2D device's) and EndDraw, which reads the
 * pixels back as dcomp.dll's own surfaces do; and surfaces showing a
 * composition swap chain of dxgi.dll's (CreateCompositionSurfaceForSwapChain).
 *
 * Changes take effect as on Windows, with no Commit: each one wakes the
 * compositor's thread, which draws every target's tree in software (pixel
 * centres mapped back through each visual's transform, brushes sampled
 * nearest, premultiplied alpha blended "source over") and copies the
 * picture into the target's window with the GDI.  The parts of the window
 * no visual covers show the window class's background colour (black when
 * it has none).  A target's window is subclassed so that the compositor
 * draws again after the window paints or changes size.
 *
 * Beyond the first versions: ICompositor2 to 8, IVisual2 to 4 and
 * ICompositionObject2 to 5 (each object answers the Windows App SDK's
 * Microsoft.UI.Composition IIDs too, which the XAML framework's source asks
 * for; DllGetActivationFactory serves Microsoft.UI.Composition.Compositor
 * as well); property sets (an object's Properties, CreatePropertySet); key
 * frame animations run by the compositor's thread, animation groups,
 * scoped and commit batches, easing functions (and their statics),
 * rectangle clips, clip and surface brush transforms (ICompositionClip2,
 * ICompositionSurfaceBrush2 and 3) and mask brushes (the XAML framework
 * draws text as its glyphs' alpha mask over a colour brush).  A compositor is a DirectComposition device as well
 * (dcomp.c's dcomp_part_new), as the XAML framework's composition tree
 * host has it, and DirectComposition surfaces can be shown in its surface
 * brushes (NovaCompositionSurfaceForDComp).
 *
 * Not implemented: expression animations' expressions (they are accepted,
 * held on their property and stopped, not evaluated), implicit
 * animations, animation controllers, effects, lights, shadows, shapes,
 * gradient, nine-grid and other brushes, brushes' and clips' AnchorPoint, perspective and 3-D rotation (only the
 * matrix's 2-D part and the Z rotation are used), clips other than inset
 * and square-cornered rectangle clips (and a clip under a rotation is its
 * bounding box), bilinear sampling and surfaces from handles.
 */
#include <windows.h>
#include <roapi.h>
#include <novadx.h>

#define COMP_API __declspec(dllexport)
typedef BYTE boolean_t;

#define E_ILLEGAL_METHOD_CALL_ ((HRESULT)0x8000000EL)
#define RO_E_CLOSED_           ((HRESULT)0x80000013L)
#define DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED_     ((HRESULT)0x88980801L)
#define DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED_ ((HRESULT)0x88980802L)
#define D3D11_BIND_SHADER_RESOURCE_  0x8
#define D3D11_BIND_RENDER_TARGET_    0x20
#define D3D11_USAGE_STAGING_         3
#define D3D11_CPU_ACCESS_READ_       0x20000
#define MAX_TARGETS 64
#define MAX_DEPTH   64

static const GUID IID_IUnknown_  = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IInspectable_ = { 0xAF86E2E0, 0xB12D, 0x4C6A, { 0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90 } };
static const GUID IID_IAgileObject_ = { 0x94EA2B94, 0xE9CC, 0x49E0, { 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90 } };
static const GUID IID_IActivationFactory_ = { 0x00000035, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IClosable_ = { 0x30D5A829, 0x7FA4, 0x4026, { 0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E } };
static const GUID IID_ICompositorInterop_ = { 0x25297D5C, 0x3AD4, 0x4C9C, { 0xB5, 0xCF, 0xE3, 0x6A, 0x38, 0x51, 0x23, 0x30 } };
static const GUID IID_ICompositorDesktopInterop_ = { 0x29E691FA, 0x4567, 0x4DCA, { 0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07 } };
static const GUID IID_IDesktopWindowTarget_ = { 0x6329D6CA, 0x3366, 0x490E, { 0x9D, 0xB3, 0x25, 0x31, 0x29, 0x29, 0xAC, 0x51 } };
static const GUID IID_ICompositionGraphicsDeviceInterop_ = { 0xA116FF71, 0xF8BF, 0x4C8A, { 0x9C, 0x98, 0x70, 0x77, 0x9A, 0x32, 0xA9, 0xC8 } };
static const GUID IID_ICompositionDrawingSurfaceInterop_ = { 0xFD04E6E3, 0xFE0C, 0x4C3C, { 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE } };
static const GUID IID_ICoreWindowStatic_ = { 0x4D239005, 0x3C2A, 0x41B1, { 0x90, 0x22, 0x53, 0x6B, 0xB9, 0xCF, 0x93, 0xB1 } };
/* Microsoft.UI.Composition's ICompositorInterop (userland/windows_ui_xaml/inc/microsoft.ui.composition.interop.h) */
static const GUID IID_ICompositorSwapChainInterop_ = { 0xA0B03594, 0xC413, 0x5F5D, { 0xBC, 0x70, 0xCE, 0x93, 0x82, 0xEB, 0x5E, 0x83 } };
static const GUID IID_ICoreWindowInterop_ = { 0x45D64A29, 0xA63E, 0x4CB6, { 0xB4, 0x98, 0x57, 0x81, 0xD2, 0x98, 0xCB, 0x4F } };
#include "composition_iids.h"
/* (NovaOS's own: hands back the object behind any of its interfaces) */
static const GUID IID_INovaCompositionObject = { 0x6e6f7661, 0x0010, 0x4358, { 0x91, 0x00, 0x63, 0x6f, 0x6d, 0x70, 0x6f, 0x73 } };

typedef struct { float X, Y; } Vec2;
typedef struct { float X, Y, Z; } Vec3;
typedef struct { float X, Y, Z, W; } Quat;
typedef struct { float m[4][4]; } Mat4;             /* M11 ... M44 */
typedef struct { BYTE A, R, G, B; } Color;          /* Windows.UI.Color */
typedef struct { float Width, Height; } SizeF;
typedef struct { float a, b, c, d, tx, ty; } Aff;   /* [x y 1] * [a b; c d; tx ty] */

/* ---- objects ---------------------------------------------------------------
 * Every object is an Obj: up to NIF interface pointers (vt[i] is the vtable
 * of the interface at slot i, so an interface pointer is &o->vt[i]), then
 * its state.  Each interface answers its IID in Windows.UI.Composition and
 * in the Windows App SDK's Microsoft.UI.Composition (the XAML framework's
 * source asks for the latter): the two have the same members in the same
 * order, but for ICompositor and ICompositionObject (Microsoft.UI's lack
 * CreateTargetForCurrentView and Dispatcher), which have a slot of their own
 * in each namespace, as do those whose DispatcherQueue is
 * Windows.System's in one and Microsoft.UI.Dispatching's in the other.
 *
 * A composition object's slots 0 and 1 are ICompositionObject and
 * IClosable, 12-17 the rest of ICompositionObject's versions (S_CO_*);
 * 2-11 depend on the kind.  The compositor's slots are its own (S_C_*). */
enum { K_COMPOSITOR, K_CONTAINER, K_SPRITE, K_COLLECTION, K_COLOR_BRUSH, K_SURFACE_BRUSH, K_INSET_CLIP,
       K_TARGET, K_GRAPHICS, K_DRAWING_SURFACE, K_CHAIN_SURFACE, K_RECT_CLIP, K_PROPSET, K_EXPRESSION, K_KEYFRAME,
       K_GROUP, K_EASING, K_BATCH, K_BATCH_ARGS, K_MASK_BRUSH, K_COUNT };
#define NIF 18
enum { S_CO_M = 12, S_CO2, S_CO3_W, S_CO3_M, S_CO4, S_CO5 };
enum { S_C_M = 4, S_C2, S_C3, S_C4, S_C5, S_C6, S_C7_W, S_C7_M, S_C8 };
/* the values property sets, animations and keyframes hold (P_*: their types) */
enum { P_NONE, P_SCALAR, P_VEC2, P_VEC3, P_VEC4, P_COLOR, P_QUAT, P_M3X2, P_M4X4, P_BOOL };
static const int p_count[] = { 0, 1, 2, 3, 4, 4, 4, 6, 16, 1 };
typedef struct { WCHAR *name; int type; float v[16]; } Prop;              /* (a colour: A, R, G, B) */
typedef struct Obj Obj;
typedef struct { WCHAR *name; int type; float v[16]; Obj *ref; } Param;   /* (ref counted) */
/* an easing function: E_* */
enum { E_LINEAR, E_CUBIC, E_STEP, E_SINE, E_CIRCLE, E_POWER, E_OTHER };
typedef struct { int type, mode; float x1, y1, x2, y2, a; int steps, first, last; boolean_t first_single, last_single; } Ease;
/* a key frame: its value, the starting value or the final value (the
 * starting value here: the expression keyframes XAML uses), or an
 * expression not evaluated (the starting value too) */
enum { KF_VALUE, KF_START, KF_FINAL, KF_EXPR };
typedef struct { float t; int kind; float v[16]; Ease ease; } KF;
typedef struct { EventRegistrationToken token; IUnknown *h; } Handler;
struct Obj {
    const void *const *vt[NIF];
    LONG refs;
    int kind;
    Obj *comp;                          /* the compositor (counted; NULL: this is it) */
    BOOL closed;
    Obj *props;                         /* Properties (counted; made when first asked for) */
    HSTRING comment;
    /* visuals */
    Obj *parent;                        /* (not counted) */
    Obj *target_of;                     /* the target this visual is the root of (not counted) */
    Obj **kids;                         /* bottom first (counted); an animation group's animations */
    int nkids, capkids;
    Vec2 anchor, size, rel_size;
    Vec3 center, offset, scale, axis, rel_offset;
    Quat orient;
    float angle, opacity;
    boolean_t visible, hit_test, snap;
    int backface, border, composite;
    Mat4 tm;
    Obj *clip, *brush;                  /* (counted) */
    Obj *for_transform;                 /* ParentForTransform (counted; kept, not used in drawing) */
    Obj *of;                            /* a collection's visual (counted) */
    /* brushes and clips */
    Color color;
    Obj *surface;                       /* (counted) */
    Obj *mask;                          /* a mask brush's Mask (counted; its Source is brush) */
    Vec2 b_anchor, b_center, b_offset, b_scale;     /* a surface brush's or a clip's transform (ICompositionSurfaceBrush2, ICompositionClip2) */
    float b_angle;
    Aff b_tm;
    boolean_t b_snap;
    int stretch, interp;
    float hratio, vratio;
    float inset[4];                     /* left, top, right, bottom (a rectangle clip's sides too) */
    Vec2 radius[4];                     /* a rectangle clip's corners (kept, not drawn) */
    /* targets */
    HWND hwnd;
    Obj *root;                          /* (counted) */
    boolean_t topmost;
    DWORD *pic;                         /* the window's picture, pw x ph */
    int pw, ph;
    /* graphics devices and drawing surfaces */
    IUnknown *d3d, *ctx;                /* (counted) */
    IUnknown *d2d;                      /* the ID2D1Device the graphics device was made on, or NULL (counted) */
    IUnknown *dc;                       /* a drawing surface's BeginDraw ID2D1DeviceContext, while drawing (counted) */
    IUnknown *tex, *staging;
    UINT w, h, format;
    int alpha;                          /* DirectXAlphaMode */
    BYTE *frame;                        /* BGRA, w * 4 bytes a row */
    BOOL drawing;
    RECT update;
    INovaSwapChain *chain;              /* (counted) */
    IUnknown *dsurf;                    /* a surface's DirectComposition surface (counted) */
    void *dcomp;                        /* the compositor's DirectComposition device (dcomp.c's dcomp_part_new), or NULL */
    /* property sets */
    Prop *propv;
    int nprops, capprops;
    /* animations (atype: the value's P_*) */
    int atype;
    WCHAR *expr, *target_name;
    Param *params;
    int nparams;
    KF *kfs;
    int nkfs;
    INT64 delay, duration;              /* (100 ns) */
    int iter_behavior, iter_count, stop_behavior, direction, delay_behavior, color_space;
    /* easing functions */
    Ease ease;
    /* scoped and commit batches; the compositor's batch now open */
    int pending;                        /* animations still running */
    boolean_t b_active, b_ended, b_fired, b_commit;
    Handler *handlers;
    int nhandlers;
    IUnknown *queue;                    /* the making thread's DispatcherQueue (counted), or NULL */
    Obj *open_batch;                    /* (not counted) */
    Obj *next_due;                      /* (the list of batches due to complete) */
    float playback_rate;
};
#define OBJ_AT(p, i) ((Obj *)((char *)(p) - (i) * sizeof(void *)))
#define IF(o, i) ((void *)&(o)->vt[i])

static CRITICAL_SECTION g_cs;
static Obj *g_targets[MAX_TARGETS];
static HANDLE g_wake;                   /* the compositor's thread's event */
static BOOL g_thread;

static void lock(void) { EnterCriticalSection(&g_cs); }
static void unlock(void) { LeaveCriticalSection(&g_cs); }

/* something shown changed: the compositor draws again */
static void schedule(void) { if (g_wake) SetEvent(g_wake); }

static HRESULT make_hstring(const WCHAR *s, HSTRING *out)
{
    if (!out) return E_POINTER;
    UINT32 n = 0;
    while (s[n]) n++;
    return WindowsCreateString(s, n, out);
}

/* (each interface's IIDs in both namespaces: a slot of their own for the
 * Microsoft.UI ones whose members differ) */
typedef struct { const GUID *iid; int slot; } Iface;
#define BOTH(n, slot) { &IIDW_##n, slot }, { &IIDM_##n, slot }
static const Iface if_compositor[] = { { &IIDW_Compositor, 0 }, { &IIDM_Compositor, S_C_M }, { &IID_IClosable_, 1 },
                                       { &IID_ICompositorInterop_, 2 }, { &IID_ICompositorSwapChainInterop_, 2 },
                                       { &IID_ICompositorDesktopInterop_, 3 }, BOTH(Compositor2, S_C2), { &IIDW_Compositor3, S_C3 },
                                       BOTH(Compositor4, S_C4), BOTH(Compositor5, S_C5), BOTH(Compositor6, S_C6),
                                       { &IIDW_Compositor7, S_C7_W }, { &IIDM_Compositor7, S_C7_M }, BOTH(Compositor8, S_C8), { 0, 0 } };
#define VISUAL_IFS BOTH(ContainerVisual, 3), BOTH(Visual, 2), BOTH(Visual2, 5), BOTH(Visual3, 6), BOTH(Visual4, 7)
static const Iface if_container[] = { VISUAL_IFS, { 0, 0 } };
static const Iface if_sprite[] = { BOTH(SpriteVisual, 4), VISUAL_IFS, { 0, 0 } };
static const Iface if_collection[] = { BOTH(VisualCollection, 2), { 0, 0 } };
static const Iface if_color[] = { BOTH(CompositionColorBrush, 3), BOTH(CompositionBrush, 2), { 0, 0 } };
static const Iface if_surface_brush[] = { BOTH(CompositionSurfaceBrush, 3), BOTH(CompositionBrush, 2), BOTH(CompositionSurfaceBrush2, 4),
                                          BOTH(CompositionSurfaceBrush3, 5), { 0, 0 } };
static const Iface if_mask_brush[] = { BOTH(CompositionMaskBrush, 3), BOTH(CompositionBrush, 2), { 0, 0 } };
static const Iface if_inset[] = { BOTH(InsetClip, 3), BOTH(CompositionClip, 2), BOTH(CompositionClip2, 4), { 0, 0 } };
static const Iface if_rect_clip[] = { BOTH(RectangleClip, 3), BOTH(CompositionClip, 2), BOTH(CompositionClip2, 4), { 0, 0 } };
static const Iface if_target[] = { { &IIDW_CompositionTarget, 2 }, { 0, 0 } };
static const Iface if_desktop_target[] = { { &IID_IDesktopWindowTarget_, 3 }, { &IIDW_CompositionTarget, 2 }, { 0, 0 } };
static const Iface if_graphics[] = { BOTH(CompositionGraphicsDevice, 2), { &IID_ICompositionGraphicsDeviceInterop_, 3 }, { 0, 0 } };
static const Iface if_drawing[] = { BOTH(CompositionDrawingSurface, 2), { &IID_ICompositionDrawingSurfaceInterop_, 3 },
                                    BOTH(CompositionSurface, 4), { 0, 0 } };
static const Iface if_chain[] = { BOTH(CompositionSurface, 2), { 0, 0 } };
static const Iface if_propset[] = { BOTH(CompositionPropertySet, 2), BOTH(CompositionPropertySet2, 3), { 0, 0 } };
#define ANIMATION_IFS BOTH(CompositionAnimation, 2), BOTH(CompositionAnimation2, 3), BOTH(CompositionAnimationBase, 4), \
                      BOTH(CompositionAnimation3, 5), BOTH(CompositionAnimation4, 6)
static const Iface if_expression[] = { BOTH(ExpressionAnimation, 7), ANIMATION_IFS, { 0, 0 } };
#define KEYFRAME_IFS(n) { BOTH(n, 10), BOTH(KeyFrameAnimation, 7), BOTH(KeyFrameAnimation2, 8), BOTH(KeyFrameAnimation3, 9), \
                          ANIMATION_IFS, { 0, 0 } }
static const Iface if_keyframes[][19] = {
    [P_SCALAR] = KEYFRAME_IFS(ScalarKeyFrameAnimation), [P_VEC2] = KEYFRAME_IFS(Vector2KeyFrameAnimation),
    [P_VEC3] = KEYFRAME_IFS(Vector3KeyFrameAnimation), [P_VEC4] = KEYFRAME_IFS(Vector4KeyFrameAnimation),
    [P_COLOR] = KEYFRAME_IFS(ColorKeyFrameAnimation), [P_QUAT] = KEYFRAME_IFS(QuaternionKeyFrameAnimation),
    [P_BOOL] = KEYFRAME_IFS(BooleanKeyFrameAnimation),
};
static const Iface if_group[] = { BOTH(CompositionAnimationGroup, 2), BOTH(CompositionAnimationBase, 3), { 0, 0 } };
static const Iface if_easing[] = { BOTH(CompositionEasingFunction, 2), { 0, 0 } };
static const Iface if_linear[] = { BOTH(LinearEasingFunction, 3), BOTH(CompositionEasingFunction, 2), { 0, 0 } };
static const Iface if_cubic[] = { BOTH(CubicBezierEasingFunction, 3), BOTH(CompositionEasingFunction, 2), { 0, 0 } };
static const Iface if_step[] = { BOTH(StepEasingFunction, 3), BOTH(CompositionEasingFunction, 2), { 0, 0 } };
static const Iface if_scoped[] = { BOTH(CompositionScopedBatch, 2), { 0, 0 } };
static const Iface if_commit[] = { BOTH(CompositionCommitBatch, 2), { 0, 0 } };
static const Iface if_batch_args[] = { BOTH(CompositionBatchCompletedEventArgs, 2), { 0, 0 } };
/* the slot of the runtime class's default interface */
static int default_slot(Obj *o)
{
    switch (o->kind) {
    case K_SPRITE: return 4;
    case K_CONTAINER: case K_COLOR_BRUSH: case K_SURFACE_BRUSH: case K_MASK_BRUSH: case K_INSET_CLIP: case K_RECT_CLIP: return 3;
    case K_TARGET: return o->vt[3] ? 3 : 2;
    case K_COMPOSITOR: return 0;
    case K_EXPRESSION: return 7;
    case K_KEYFRAME: return 10;
    case K_EASING: return o->vt[3] ? 3 : 2;
    }
    return 2;
}
static const Iface *ifaces(Obj *o)
{
    switch (o->kind) {
    case K_COMPOSITOR: return if_compositor;
    case K_CONTAINER: return if_container;
    case K_SPRITE: return if_sprite;
    case K_COLLECTION: return if_collection;
    case K_COLOR_BRUSH: return if_color;
    case K_SURFACE_BRUSH: return if_surface_brush;
    case K_MASK_BRUSH: return if_mask_brush;
    case K_INSET_CLIP: return if_inset;
    case K_RECT_CLIP: return if_rect_clip;
    case K_TARGET: return o->vt[3] ? if_desktop_target : if_target;
    case K_GRAPHICS: return if_graphics;
    case K_DRAWING_SURFACE: return if_drawing;
    case K_CHAIN_SURFACE: return if_chain;
    case K_PROPSET: return if_propset;
    case K_EXPRESSION: return if_expression;
    case K_KEYFRAME: return if_keyframes[o->atype];
    case K_GROUP: return if_group;
    case K_EASING: return o->ease.type == E_LINEAR ? if_linear : o->ease.type == E_CUBIC ? if_cubic :
                          o->ease.type == E_STEP ? if_step : if_easing;
    case K_BATCH: return o->b_commit ? if_commit : if_scoped;
    case K_BATCH_ARGS: return if_batch_args;
    }
    return if_target;
}

static ULONG obj_addref(Obj *o) { return (ULONG)InterlockedIncrement(&o->refs); }
static void obj_free(Obj *o);
static ULONG obj_release(Obj *o)
{
    LONG r = InterlockedDecrement(&o->refs);
    if (!r) obj_free(o);
    return (ULONG)r;
}
/* ICompositionObject's versions, in each namespace */
static const Iface if_object[] = { { &IIDW_CompositionObject, 0 }, { &IIDM_CompositionObject, S_CO_M }, { &IID_IClosable_, 1 },
                                   BOTH(CompositionObject2, S_CO2), { &IIDW_CompositionObject3, S_CO3_W },
                                   { &IIDM_CompositionObject3, S_CO3_M }, BOTH(CompositionObject4, S_CO4),
                                   BOTH(CompositionObject5, S_CO5), { 0, 0 } };
static int find_slot(const Iface *f, REFIID iid)
{
    for (; f->iid; f++)
        if ((f->iid->Data1 | f->iid->Data2 | f->iid->Data3) && IsEqualGUID(iid, f->iid))   /* (none: GUID_NULL) */ return f->slot;
    return -1;
}
void *dcomp_part_new(IUnknown *outer);  /* (dcomp.c) */
void dcomp_part_free(void *part);
BOOL dcomp_part_iid(const GUID *iid);
HRESULT dcomp_part_qi(void *part, const GUID *iid, void **out);
const BYTE *dcomp_surface_frame(IUnknown *unk, UINT *pitch, UINT *w, UINT *h, BOOL *opaque);
static HRESULT obj_qi(Obj *o, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    /* a compositor is a DirectComposition device too (made when first asked
     * for), as the XAML framework's composition tree host has it */
    if (o->kind == K_COMPOSITOR && dcomp_part_iid(iid)) {
        lock();
        if (!o->dcomp) o->dcomp = dcomp_part_new((IUnknown *)IF(o, 0));
        void *part = o->dcomp;
        unlock();
        return part ? dcomp_part_qi(part, iid, out) : E_OUTOFMEMORY;
    }
    if (IsEqualGUID(iid, &IID_INovaCompositionObject)) { *out = o; obj_addref(o); return S_OK; }
    int slot = -1;
    if (IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_IInspectable_) || IsEqualGUID(iid, &IID_IAgileObject_))
        slot = default_slot(o);
    else {
        if (o->kind != K_COMPOSITOR) slot = find_slot(if_object, iid);
        if (slot < 0) slot = find_slot(ifaces(o), iid);
    }
    if (slot < 0 || !o->vt[slot]) return E_NOINTERFACE;
    *out = IF(o, slot);
    obj_addref(o);
    return S_OK;
}
static HRESULT obj_name(Obj *o, HSTRING *n)
{
    static const WCHAR *const names[K_COUNT] = {
        L"Windows.UI.Composition.Compositor", L"Windows.UI.Composition.ContainerVisual", L"Windows.UI.Composition.SpriteVisual",
        L"Windows.UI.Composition.VisualCollection", L"Windows.UI.Composition.CompositionColorBrush",
        L"Windows.UI.Composition.CompositionSurfaceBrush", L"Windows.UI.Composition.InsetClip",
        L"Windows.UI.Composition.CompositionTarget", L"Windows.UI.Composition.CompositionGraphicsDevice",
        L"Windows.UI.Composition.CompositionDrawingSurface", L"Windows.UI.Composition.CompositionVisualSurface",
        L"Windows.UI.Composition.RectangleClip", L"Windows.UI.Composition.CompositionPropertySet",
        L"Windows.UI.Composition.ExpressionAnimation", L"Windows.UI.Composition.KeyFrameAnimation",
        L"Windows.UI.Composition.CompositionAnimationGroup", L"Windows.UI.Composition.CompositionEasingFunction",
        L"Windows.UI.Composition.CompositionScopedBatch", L"Windows.UI.Composition.CompositionBatchCompletedEventArgs",
        L"Windows.UI.Composition.CompositionMaskBrush",
    };
    static const WCHAR *const keyframes[] = {
        [P_SCALAR] = L"Windows.UI.Composition.ScalarKeyFrameAnimation", [P_VEC2] = L"Windows.UI.Composition.Vector2KeyFrameAnimation",
        [P_VEC3] = L"Windows.UI.Composition.Vector3KeyFrameAnimation", [P_VEC4] = L"Windows.UI.Composition.Vector4KeyFrameAnimation",
        [P_COLOR] = L"Windows.UI.Composition.ColorKeyFrameAnimation", [P_QUAT] = L"Windows.UI.Composition.QuaternionKeyFrameAnimation",
        [P_M3X2] = 0, [P_M4X4] = 0, [P_BOOL] = L"Windows.UI.Composition.BooleanKeyFrameAnimation",
    };
    static const WCHAR *const easings[] = {
        L"Windows.UI.Composition.LinearEasingFunction", L"Windows.UI.Composition.CubicBezierEasingFunction",
        L"Windows.UI.Composition.StepEasingFunction", L"Windows.UI.Composition.SineEasingFunction",
        L"Windows.UI.Composition.CircleEasingFunction", L"Windows.UI.Composition.PowerEasingFunction",
        L"Windows.UI.Composition.CompositionEasingFunction",
    };
    if (o->kind == K_TARGET && o->vt[3]) return make_hstring(L"Windows.UI.Composition.Desktop.DesktopWindowTarget", n);
    if (o->kind == K_KEYFRAME) return make_hstring(keyframes[o->atype], n);
    if (o->kind == K_EASING) return make_hstring(easings[o->ease.type], n);
    if (o->kind == K_BATCH && o->b_commit) return make_hstring(L"Windows.UI.Composition.CompositionCommitBatch", n);
    return make_hstring(names[o->kind], n);
}
static HRESULT STDMETHODCALLTYPE no_iids(void *p, ULONG *n, GUID **iids) { (void)p; if (!n || !iids) return E_POINTER; *n = 0; *iids = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE base_trust(void *p, int *t) { (void)p; if (!t) return E_POINTER; *t = BaseTrust; return S_OK; }

#define THUNKS(i) \
    static HRESULT STDMETHODCALLTYPE qi##i(void *p, REFIID iid, void **out) { return obj_qi(OBJ_AT(p, i), iid, out); } \
    static ULONG STDMETHODCALLTYPE ar##i(void *p) { return obj_addref(OBJ_AT(p, i)); } \
    static ULONG STDMETHODCALLTYPE rl##i(void *p) { return obj_release(OBJ_AT(p, i)); } \
    static HRESULT STDMETHODCALLTYPE nm##i(void *p, HSTRING *s) { return obj_name(OBJ_AT(p, i), s); }
THUNKS(0) THUNKS(1) THUNKS(2) THUNKS(3) THUNKS(4) THUNKS(5) THUNKS(6) THUNKS(7) THUNKS(8) THUNKS(9) THUNKS(10) THUNKS(11)
THUNKS(12) THUNKS(13) THUNKS(14) THUNKS(15) THUNKS(16) THUNKS(17)
#define BASE(i) (void *)qi##i, (void *)ar##i, (void *)rl##i, (void *)no_iids, (void *)nm##i, (void *)base_trust
#define UNK(i) (void *)qi##i, (void *)ar##i, (void *)rl##i

/* The object behind an interface pointer of ours (counted), or NULL */
static Obj *obj_of(void *p, int kind1, int kind2)
{
    Obj *o = 0;
    if (!p || FAILED(((HRESULT (STDMETHODCALLTYPE *)(void *, REFIID, void **))(*(void ***)p)[0])(p, &IID_INovaCompositionObject, (void **)&o)) || !o)
        return 0;
    if (kind1 >= 0 && o->kind != kind1 && o->kind != kind2) { obj_release(o); return 0; }
    return o;
}
static BOOL is_visual(Obj *o) { return o && (o->kind == K_CONTAINER || o->kind == K_SPRITE); }
/* the brush behind an interface pointer of ours (counted), or NULL */
static Obj *brush_of(void *p)
{
    Obj *b = obj_of(p, -1, -1);
    if (b && b->kind != K_COLOR_BRUSH && b->kind != K_SURFACE_BRUSH && b->kind != K_MASK_BRUSH) { obj_release(b); b = 0; }
    return b;
}

/* *@out: @o's interface at @slot, counted (NULL stays NULL) */
static HRESULT give(Obj *o, int slot, void **out)
{
    if (!out) return E_POINTER;
    *out = o ? IF(o, slot) : 0;
    if (o) obj_addref(o);
    return S_OK;
}
/* replaces the counted *@field with @o (counted by the caller) */
static void swap_ref(Obj **field, Obj *o)
{
    lock();
    Obj *old = *field;
    *field = o;
    unlock();
    if (old) obj_release(old);
    schedule();
}

static const void *const co_vt[], *const closable_vt[], *const co_m_vt[], *const co2_vt[], *const co3_w_vt[], *const co3_m_vt[],
                  *const co4_vt[], *const co5_vt[];
static Obj *obj_new(Obj *comp, int kind)
{
    Obj *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
    if (!o) return 0;
    o->refs = 1;
    o->kind = kind;
    if (comp) { o->comp = comp; obj_addref(comp); }
    o->b_scale.X = o->b_scale.Y = 1.0f;
    o->b_tm.a = o->b_tm.d = 1.0f;
    if (kind != K_COMPOSITOR) {
        o->vt[0] = co_vt; o->vt[1] = closable_vt; o->vt[S_CO_M] = co_m_vt; o->vt[S_CO2] = co2_vt;
        o->vt[S_CO3_W] = co3_w_vt; o->vt[S_CO3_M] = co3_m_vt; o->vt[S_CO4] = co4_vt; o->vt[S_CO5] = co5_vt;
    }
    return o;
}

/* ---- maths ----------------------------------------------------------------- */
static double c_sin(double x)
{
    const double pi = 3.14159265358979323846;
    int k = (int)(x / (2 * pi));
    x -= k * 2 * pi;
    if (x > pi) x -= 2 * pi;
    if (x < -pi) x += 2 * pi;
    if (x > pi / 2) x = pi - x;
    if (x < -pi / 2) x = -pi - x;
    double x2 = x * x, t = x, s = x;
    for (int i = 1; i <= 6; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
static double c_cos(double x) { return c_sin(x + 3.14159265358979323846 / 2); }
static float c_abs(float f) { return f < 0 ? -f : f; }
static float c_sqrt(float x)
{
    if (!(x > 0)) return 0;
    float r = x > 1 ? x : 1;
    for (int i = 0; i < 40; i++) r = 0.5f * (r + x / r);
    return r;
}

static Aff aff_mul(Aff p, Aff q)                    /* p, then q */
{
    Aff r = { p.a * q.a + p.b * q.c, p.a * q.b + p.b * q.d, p.c * q.a + p.d * q.c, p.c * q.b + p.d * q.d,
              p.tx * q.a + p.ty * q.c + q.tx, p.tx * q.b + p.ty * q.d + q.ty };
    return r;
}
static Aff aff_move(float x, float y) { Aff r = { 1, 0, 0, 1, x, y }; return r; }
static void aff_apply(const Aff *m, float x, float y, float *ox, float *oy)
{
    *ox = x * m->a + y * m->c + m->tx;
    *oy = x * m->b + y * m->d + m->ty;
}

/* A visual's size: its Size, and its RelativeSizeAdjustment of its
 * parent's (@psize) */
static Vec2 visual_size(Obj *v, Vec2 psize)
{
    Vec2 s = { v->size.X + v->rel_size.X * psize.X, v->size.Y + v->rel_size.Y * psize.Y };
    return s;
}
/* A visual's own transform: about its centre point scaled, rotated and
 * transformed by its matrix, then moved to its offset (and its
 * RelativeOffsetAdjustment of its parent's size) with its anchor point
 * there */
static Aff visual_matrix(Obj *v, Vec2 size, Vec2 psize)
{
    Aff m = aff_move(-v->center.X, -v->center.Y);
    Aff s = { v->scale.X, 0, 0, v->scale.Y, 0, 0 };
    m = aff_mul(m, s);
    if (v->angle != 0.0f && v->axis.X == 0.0f && v->axis.Y == 0.0f) {
        float k = (float)c_cos(v->axis.Z < 0 ? -v->angle : v->angle), n = (float)c_sin(v->axis.Z < 0 ? -v->angle : v->angle);
        Aff r = { k, n, -n, k, 0, 0 };
        m = aff_mul(m, r);
    }
    m = aff_mul(m, aff_move(v->center.X, v->center.Y));
    Aff t = { v->tm.m[0][0], v->tm.m[0][1], v->tm.m[1][0], v->tm.m[1][1], v->tm.m[3][0], v->tm.m[3][1] };
    m = aff_mul(m, t);
    return aff_mul(m, aff_move(v->offset.X + v->rel_offset.X * psize.X - v->anchor.X * size.X,
                               v->offset.Y + v->rel_offset.Y * psize.Y - v->anchor.Y * size.Y));
}

/* the bounding box (pixels) of the rectangle (@l, @t)-(@r, @b) under @m */
static RECT aff_box(const Aff *m, float l, float t, float r, float b)
{
    float xs[4], ys[4];
    aff_apply(m, l, t, &xs[0], &ys[0]);
    aff_apply(m, r, t, &xs[1], &ys[1]);
    aff_apply(m, l, b, &xs[2], &ys[2]);
    aff_apply(m, r, b, &xs[3], &ys[3]);
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < x0) x0 = xs[i];
        if (xs[i] > x1) x1 = xs[i];
        if (ys[i] < y0) y0 = ys[i];
        if (ys[i] > y1) y1 = ys[i];
    }
    const float big = 1.0e6f;
    if (x0 < -big) x0 = -big;
    if (y0 < -big) y0 = -big;
    if (x1 > big) x1 = big;
    if (y1 > big) y1 = big;
    RECT box = { (LONG)(x0 + 0.5f) - (x0 + 0.5f < 0), (LONG)(y0 + 0.5f) - (y0 + 0.5f < 0), (LONG)(x1 + 0.5f), (LONG)(y1 + 0.5f) };
    return box;
}

/* ---- drawing ------------------------------------------------------------------ */
static DWORD premul(Color c, float op)
{
    UINT a = (UINT)(int)(c.A * op + 0.5f);
    return a << 24 | (c.R * a + 127) / 255 << 16 | (c.G * a + 127) / 255 << 8 | (c.B * a + 127) / 255;
}
/* source over: @s (premultiplied BGRA) onto @d */
static DWORD over(DWORD s, DWORD d)
{
    UINT a = s >> 24;
    if (a == 255) return s;
    if (!a) return d;
    UINT k = 255 - a;
    UINT rb = ((d & 0xFF00FF) * k + 0x800080) >> 8 & 0xFF00FF;
    UINT g = ((d >> 8 & 0xFF00FF) * k + 0x800080) & 0xFF00FF00;
    return s + (rb | g);
}
static DWORD scale_px(DWORD s, UINT op)             /* (op 0-256) */
{
    if (op >= 256) return s;
    return ((s & 0xFF00FF) * op >> 8 & 0xFF00FF) | ((s >> 8 & 0xFF00FF) * op & 0xFF00FF00);
}

/* A surface brush's surface: its pixels (BGRA), size and whether its alpha
 * means anything */
static const BYTE *surface_pixels(Obj *s, UINT *pitch, UINT *w, UINT *h, BOOL *opaque)
{
    if (!s) return 0;
    if (s->kind == K_DRAWING_SURFACE && s->frame) {
        *w = s->w; *h = s->h; *pitch = s->w * 4;
        *opaque = s->alpha == 3 || s->format == NDXGI_FORMAT_B8G8R8X8_UNORM;
        return s->frame;
    }
    if (s->kind == K_CHAIN_SURFACE && s->dsurf) return dcomp_surface_frame(s->dsurf, pitch, w, h, opaque);
    if (s->kind == K_CHAIN_SURFACE && s->chain) {
        *opaque = TRUE;                 /* (a swap chain's frames are shown opaque) */
        return s->chain->lpVtbl->Frame(s->chain, pitch, w, h);
    }
    return 0;
}

/* a surface brush's or a clip's own transform (ICompositionSurfaceBrush2,
 * ICompositionClip2) */
static Aff xform(const Obj *o)
{
    float k = (float)c_cos(o->b_angle), n = (float)c_sin(o->b_angle);
    Aff rs = { o->b_scale.X * k, o->b_scale.X * n, -o->b_scale.Y * n, o->b_scale.Y * k, 0, 0 };
    return aff_mul(aff_mul(aff_mul(aff_move(-o->b_center.X, -o->b_center.Y), rs),
                           aff_move(o->b_center.X + o->b_offset.X, o->b_center.Y + o->b_offset.Y)), o->b_tm);
}

/* where a brush's pixels come from, for a visual @vs in size: a colour
 * (premultiplied), or a surface's pixels placed in the visual by the
 * brush's stretch and alignment */
typedef struct {
    DWORD solid;
    const BYTE *bits;
    UINT pitch, sw, sh;
    BOOL opaque;
    float cx, cy, cw, ch;               /* the surface's place in the visual */
    BOOL moved;                         /* the brush's transform, inverted (visual to content) */
    Aff inv;
} Samp;
static BOOL samp_init(Obj *b, Vec2 vs, Samp *s)
{
    s->solid = 0; s->bits = 0; s->pitch = s->sw = s->sh = 0; s->opaque = FALSE; s->moved = FALSE;
    s->cx = s->cy = 0; s->cw = vs.X; s->ch = vs.Y;
    if (!b) return FALSE;
    if (b->kind == K_COLOR_BRUSH) {
        s->solid = premul(b->color, 1.0f);
        return (s->solid >> 24) != 0;
    }
    if (b->kind != K_SURFACE_BRUSH) return FALSE;
    s->bits = surface_pixels(b->surface, &s->pitch, &s->sw, &s->sh, &s->opaque);
    if (!s->bits || !s->sw || !s->sh) return FALSE;
    float kx = vs.X / s->sw, ky = vs.Y / s->sh;
    switch (b->stretch) {
    case 0: s->cw = (float)s->sw; s->ch = (float)s->sh; break;                      /* None */
    case 2: { float k = kx < ky ? kx : ky; s->cw = s->sw * k; s->ch = s->sh * k; break; }   /* Uniform */
    case 3: { float k = kx > ky ? kx : ky; s->cw = s->sw * k; s->ch = s->sh * k; break; }   /* UniformToFill */
    }
    s->cx = (vs.X - s->cw) * b->hratio;
    s->cy = (vs.Y - s->ch) * b->vratio;
    Aff m = xform(b);
    s->moved = m.a != 1 || m.b != 0 || m.c != 0 || m.d != 1 || m.tx != 0 || m.ty != 0;
    if (s->moved) {
        float det = m.a * m.d - m.b * m.c;
        if (c_abs(det) < 1e-9f) return FALSE;
        Aff inv = { m.d / det, -m.b / det, -m.c / det, m.a / det, 0, 0 };
        inv.tx = -(m.tx * inv.a + m.ty * inv.c);
        inv.ty = -(m.tx * inv.b + m.ty * inv.d);
        s->inv = inv;
    }
    return TRUE;
}
/* the brush's pixel at (@u, @w) in the visual (premultiplied; 0: none) */
static DWORD samp_at(const Samp *s, float u, float w)
{
    if (!s->bits) return s->solid;
    if (s->moved) { float x = u, y = w; aff_apply(&s->inv, x, y, &u, &w); }
    float su = (u - s->cx) * s->sw / s->cw, sv = (w - s->cy) * s->sh / s->ch;
    if (su < 0 || sv < 0 || su >= s->sw || sv >= s->sh) return 0;
    DWORD px = ((const DWORD *)(s->bits + (size_t)(int)sv * s->pitch))[(int)su];
    return s->opaque ? px | 0xFF000000u : px;
}

/* @v (a sprite, @vs in size) filled with its brush in @pic (@pw x @ph),
 * through @m, inside @clip, at opacity @op */
static void fill(Obj *v, Vec2 vs, const Aff *m, RECT clip, float op, DWORD *pic, int pw, int ph)
{
    Obj *b = v->brush;
    if (!b || vs.X <= 0 || vs.Y <= 0) return;
    float det = m->a * m->d - m->b * m->c;
    if (c_abs(det) < 1e-9f) return;
    RECT box = aff_box(m, 0, 0, vs.X, vs.Y), all = { 0, 0, pw, ph };
    if (!IntersectRect(&box, &box, &clip) || !IntersectRect(&box, &box, &all)) return;
    Samp src, mask;
    BOOL masked = b->kind == K_MASK_BRUSH;
    if (!samp_init(masked ? b->brush : b, vs, &src)) return;
    if (masked && !samp_init(b->mask, vs, &mask)) return;
    UINT opi = (UINT)(int)(op * 256.0f + 0.5f);
    /* each pixel's centre, back in the visual's space */
    float ia = m->d / det, ib = -m->b / det, ic = -m->c / det, id = m->a / det;
    for (LONG y = box.top; y < box.bottom; y++) {
        float X = box.left + 0.5f - m->tx, Y = y + 0.5f - m->ty;
        float u = X * ia + Y * ic, w = X * ib + Y * id;
        DWORD *row = pic + (size_t)y * pw;
        for (LONG x = box.left; x < box.right; x++, u += ia, w += ib) {
            if (u < 0 || w < 0 || u >= vs.X || w >= vs.Y) continue;
            DWORD s = samp_at(&src, u, w);
            if (masked && s) {
                UINT a = samp_at(&mask, u, w) >> 24;
                s = a ? scale_px(s, a + (a >> 7)) : 0;
            }
            if (!s) continue;
            row[x] = over(scale_px(s, opi), row[x]);
        }
    }
}

/* (@psize: the parent's size, or the window's for a target's root) */
static void draw_visual(Obj *v, Aff parent, Vec2 psize, RECT clip, float op, DWORD *pic, int pw, int ph, int depth)
{
    if (!v || depth > MAX_DEPTH || !v->visible || v->opacity <= 0.0f) return;
    Vec2 vs = visual_size(v, psize);
    Aff m = aff_mul(visual_matrix(v, vs, psize), parent);
    op *= v->opacity > 1.0f ? 1.0f : v->opacity;
    if (v->clip) {
        const float *k = v->clip->inset;
        Aff cm = aff_mul(xform(v->clip), m);
        RECT c = v->clip->kind == K_RECT_CLIP ? aff_box(&cm, k[0], k[1], k[2], k[3])       /* (left, top, right, bottom) */
                                              : aff_box(&cm, k[0], k[1], vs.X - k[2], vs.Y - k[3]);
        if (!IntersectRect(&clip, &clip, &c)) return;
    }
    if (v->kind == K_SPRITE) fill(v, vs, &m, clip, op, pic, pw, ph);
    for (int i = 0; i < v->nkids; i++) draw_visual(v->kids[i], m, vs, clip, op, pic, pw, ph, depth + 1);
}

static DWORD window_background(HWND h)
{
    HBRUSH br = (HBRUSH)GetClassLongPtrW(h, GCLP_HBRBACKGROUND);
    if (!br) return 0xFF000000u;
    if ((ULONG_PTR)br <= 31) {
        COLORREF c = GetSysColor((int)(ULONG_PTR)br - 1);
        return 0xFF000000u | GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c);
    }
    LOGBRUSH lb;
    if (GetObjectW(br, sizeof lb, &lb) != sizeof lb || lb.lbStyle != 0 /* BS_SOLID */) return 0xFF000000u;
    return 0xFF000000u | GetRValue(lb.lbColor) << 16 | GetGValue(lb.lbColor) << 8 | GetBValue(lb.lbColor);
}

/* @t's tree drawn into its window (under the lock) */
static void target_draw(Obj *t)
{
    RECT cr;
    if (!t->hwnd || !IsWindow(t->hwnd) || !IsWindowVisible(t->hwnd) || !GetClientRect(t->hwnd, &cr)) return;
    int w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0) return;
    if (w != t->pw || h != t->ph || !t->pic) {
        HeapFree(GetProcessHeap(), 0, t->pic);
        t->pic = HeapAlloc(GetProcessHeap(), 0, (size_t)w * h * 4);
        t->pw = t->pic ? w : 0; t->ph = t->pic ? h : 0;
        if (!t->pic) return;
    }
    DWORD bg = window_background(t->hwnd);
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) t->pic[i] = bg;
    Aff id = { 1, 0, 0, 1, 0, 0 };
    RECT all = { 0, 0, w, h };
    Vec2 wsize = { (float)w, (float)h };
    draw_visual(t->root, id, wsize, all, 1.0f, t->pic, w, h, 0);
    HDC dc = GetDC(t->hwnd);
    if (!dc) return;
    novadx_blit(dc, 0, 0, (const BYTE *)t->pic, (UINT)w * 4, (UINT)w, (UINT)h, NULL);
    ReleaseDC(t->hwnd, dc);
}

/* The compositor's thread: draws every target after a change (several
 * changes made together are drawn once), and while key frame animations
 * run, moves them on and draws about every 16 ms; then raises the
 * Completed events of the batches whose animations have all finished */
static BOOL anim_tick(void);
static void batches_fire(void);
static DWORD WINAPI compositor_thread(void *arg)
{
    (void)arg;
    BOOL ticking = FALSE;
    for (;;) {
        if (WaitForSingleObject(g_wake, ticking ? 16 : INFINITE) == WAIT_OBJECT_0) Sleep(2);
        lock();
        ticking = anim_tick();
        for (int i = 0; i < MAX_TARGETS; i++) if (g_targets[i]) target_draw(g_targets[i]);
        unlock();
        batches_fire();
    }
    return 0;
}
static void ensure_thread(void)
{
    lock();
    if (!g_thread) {
        HANDLE th = CreateThread(0, 0, compositor_thread, 0, 0, 0);
        if (th) { CloseHandle(th); g_thread = TRUE; }
    }
    unlock();
}

/* ---- targets' windows -----------------------------------------------------------
 * A target's window is subclassed: after it paints, changes size or is
 * shown, the compositor draws again */
typedef struct { HWND hwnd; WNDPROC old; } Sub;
static Sub g_subs[MAX_TARGETS];
static LRESULT CALLBACK target_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = 0;
    lock();
    for (int i = 0; i < MAX_TARGETS; i++) if (g_subs[i].hwnd == h) { old = g_subs[i].old; break; }
    unlock();
    LRESULT r = old ? CallWindowProcW(old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: case WM_SIZE: case WM_SHOWWINDOW: case WM_WINDOWPOSCHANGED: case WM_ERASEBKGND:
        schedule();
        break;
    case WM_NCDESTROY:
        lock();
        for (int i = 0; i < MAX_TARGETS; i++) if (g_subs[i].hwnd == h) g_subs[i].hwnd = 0;
        unlock();
        break;
    }
    return r;
}
static void subclass(HWND h)
{
    for (int i = 0; i < MAX_TARGETS; i++) if (g_subs[i].hwnd == h) return;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (!g_subs[i].hwnd) {
            g_subs[i].old = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
            g_subs[i].hwnd = h;
            SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)target_proc);
            return;
        }
}
static void unsubclass(HWND h)
{
    for (int i = 0; i < MAX_TARGETS; i++) if (g_targets[i] && g_targets[i]->hwnd == h) return;     /* (another target's) */
    for (int i = 0; i < MAX_TARGETS; i++)
        if (g_subs[i].hwnd == h) {
            if (IsWindow(h) && (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) == target_proc)
                SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)g_subs[i].old);
            g_subs[i].hwnd = 0;
        }
}

/* ---- freeing ------------------------------------------------------------------- */
static void visual_unhook(Obj *kid) { kid->parent = 0; }
static void obj_free(Obj *o)
{
    Obj *drop[8];
    int n = 0;
    lock();
    if (o->kind == K_BATCH && o->comp && o->comp->open_batch == o) o->comp->open_batch = 0;
    for (int i = 0; i < o->nkids; i++) visual_unhook(o->kids[i]);
    if (o->kind == K_TARGET) {
        for (int i = 0; i < MAX_TARGETS; i++) if (g_targets[i] == o) g_targets[i] = 0;
        if (o->hwnd) unsubclass(o->hwnd);
        if (o->root) o->root->target_of = 0;
    }
    if (o->chain) o->chain->lpVtbl->SetSink(o->chain, 0, 0);
    drop[n++] = o->clip; drop[n++] = o->brush; drop[n++] = o->of; drop[n++] = o->surface; drop[n++] = o->root;
    drop[n++] = o->props; drop[n++] = o->for_transform; drop[n++] = o->mask;
    unlock();
    for (int i = 0; i < o->nprops; i++) HeapFree(GetProcessHeap(), 0, o->propv[i].name);
    HeapFree(GetProcessHeap(), 0, o->propv);
    for (int i = 0; i < o->nparams; i++) {
        HeapFree(GetProcessHeap(), 0, o->params[i].name);
        if (o->params[i].ref) obj_release(o->params[i].ref);
    }
    HeapFree(GetProcessHeap(), 0, o->params);
    HeapFree(GetProcessHeap(), 0, o->kfs);
    HeapFree(GetProcessHeap(), 0, o->expr);
    HeapFree(GetProcessHeap(), 0, o->target_name);
    for (int i = 0; i < o->nhandlers; i++) o->handlers[i].h->lpVtbl->Release(o->handlers[i].h);
    HeapFree(GetProcessHeap(), 0, o->handlers);
    if (o->queue) o->queue->lpVtbl->Release(o->queue);
    if (o->comment) WindowsDeleteString(o->comment);
    for (int i = 0; i < o->nkids; i++) obj_release(o->kids[i]);
    for (int i = 0; i < n; i++) if (drop[i]) obj_release(drop[i]);
    HeapFree(GetProcessHeap(), 0, o->kids);
    HeapFree(GetProcessHeap(), 0, o->pic);
    HeapFree(GetProcessHeap(), 0, o->frame);
    if (o->chain) o->chain->lpVtbl->Release(o->chain);
    if (o->dsurf) o->dsurf->lpVtbl->Release(o->dsurf);
    dcomp_part_free(o->dcomp);
    if (o->tex) o->tex->lpVtbl->Release(o->tex);
    if (o->staging) o->staging->lpVtbl->Release(o->staging);
    if (o->dc) novadx_d2d_end(o->dc);
    if (o->ctx) o->ctx->lpVtbl->Release(o->ctx);
    if (o->d3d) o->d3d->lpVtbl->Release(o->d3d);
    if (o->d2d) o->d2d->lpVtbl->Release(o->d2d);
    if (o->comp) obj_release(o->comp);
    HeapFree(GetProcessHeap(), 0, o);
    schedule();
}

/* ---- ICompositionObject and IClosable (slots 0 and 1) ------------------------------ */
static HRESULT obj_properties(Obj *o, void **out);
static HRESULT anim_start(Obj *o, HSTRING name, void *anim);
static HRESULT anim_stop(Obj *o, HSTRING name);
static HRESULT group_run(Obj *o, void *group, BOOL start);
static HRESULT thread_queue(BOOL ms, void **out);
static HRESULT comment_get(Obj *o, HSTRING *out) { if (!out) return E_POINTER; lock(); HRESULT hr = WindowsDuplicateString(o->comment, out); unlock(); return hr; }
static HRESULT comment_put(Obj *o, HSTRING s)
{
    HSTRING d = 0;
    HRESULT hr = WindowsDuplicateString(s, &d);
    if (FAILED(hr)) return hr;
    lock();
    HSTRING old = o->comment;
    o->comment = d;
    unlock();
    if (old) WindowsDeleteString(old);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE co_compositor(void *p, void **out) { Obj *o = OBJ_AT(p, 0); return give(o->comp, 0, out); }
static HRESULT STDMETHODCALLTYPE co_dispatcher(void *p, void **out) { (void)p; if (!out) return E_POINTER; *out = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE co_properties(void *p, void **out) { return obj_properties(OBJ_AT(p, 0), out); }
static HRESULT STDMETHODCALLTYPE co_start(void *p, HSTRING name, void *anim) { return anim_start(OBJ_AT(p, 0), name, anim); }
static HRESULT STDMETHODCALLTYPE co_stop(void *p, HSTRING name) { return anim_stop(OBJ_AT(p, 0), name); }
static const void *const co_vt[] = { BASE(0), (void *)co_compositor, (void *)co_dispatcher, (void *)co_properties,
                                     (void *)co_start, (void *)co_stop };
/* Microsoft.UI's ICompositionObject: no Dispatcher; its Compositor is Microsoft.UI's */
static HRESULT STDMETHODCALLTYPE com_compositor(void *p, void **out) { return give(OBJ_AT(p, S_CO_M)->comp, S_C_M, out); }
static HRESULT STDMETHODCALLTYPE com_properties(void *p, void **out) { return obj_properties(OBJ_AT(p, S_CO_M), out); }
static HRESULT STDMETHODCALLTYPE com_start(void *p, HSTRING name, void *anim) { return anim_start(OBJ_AT(p, S_CO_M), name, anim); }
static HRESULT STDMETHODCALLTYPE com_stop(void *p, HSTRING name) { return anim_stop(OBJ_AT(p, S_CO_M), name); }
static const void *const co_m_vt[] = { BASE(12), (void *)com_compositor, (void *)com_properties, (void *)com_start, (void *)com_stop };
_Static_assert(S_CO_M == 12, "BASE(12) is S_CO_M's");
/* ICompositionObject2: Comment, ImplicitAnimations (none here), StartAnimationGroup, StopAnimationGroup */
static HRESULT STDMETHODCALLTYPE co2_get_comment(void *p, HSTRING *out) { return comment_get(OBJ_AT(p, S_CO2), out); }
static HRESULT STDMETHODCALLTYPE co2_put_comment(void *p, HSTRING s) { return comment_put(OBJ_AT(p, S_CO2), s); }
static HRESULT STDMETHODCALLTYPE co2_get_implicit(void *p, void **out) { (void)p; if (!out) return E_POINTER; *out = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE co2_put_implicit(void *p, void *c) { (void)p; return c ? E_NOTIMPL : S_OK; }
static HRESULT STDMETHODCALLTYPE co2_start_group(void *p, void *g) { return group_run(OBJ_AT(p, S_CO2), g, TRUE); }
static HRESULT STDMETHODCALLTYPE co2_stop_group(void *p, void *g) { return group_run(OBJ_AT(p, S_CO2), g, FALSE); }
static const void *const co2_vt[] = { BASE(13), (void *)co2_get_comment, (void *)co2_put_comment, (void *)co2_get_implicit,
                                      (void *)co2_put_implicit, (void *)co2_start_group, (void *)co2_stop_group };
/* ICompositionObject3: DispatcherQueue (the calling thread's: Windows.System's or Microsoft.UI.Dispatching's) */
static HRESULT STDMETHODCALLTYPE co3_w_queue(void *p, void **out) { (void)p; return thread_queue(FALSE, out); }
static HRESULT STDMETHODCALLTYPE co3_m_queue(void *p, void **out) { (void)p; return thread_queue(TRUE, out); }
static const void *const co3_w_vt[] = { BASE(14), (void *)co3_w_queue };
static const void *const co3_m_vt[] = { BASE(15), (void *)co3_m_queue };
/* ICompositionObject4: TryGetAnimationController (no controllers here);
 * ICompositionObject5: StartAnimation with a controller (not used) */
static HRESULT STDMETHODCALLTYPE co4_controller(void *p, HSTRING name, void **out) { (void)p; (void)name; if (!out) return E_POINTER; *out = 0; return S_OK; }
static const void *const co4_vt[] = { BASE(16), (void *)co4_controller };
static HRESULT STDMETHODCALLTYPE co5_start(void *p, HSTRING name, void *anim, void *controller)
{
    (void)controller;
    return anim_start(OBJ_AT(p, S_CO5), name, anim);
}
static const void *const co5_vt[] = { BASE(17), (void *)co5_start };
_Static_assert(S_CO2 == 13 && S_CO3_W == 14 && S_CO3_M == 15 && S_CO4 == 16 && S_CO5 == 17, "the BASE(n) above");
/* Close: a target stops showing its tree; the rest lets go of what it holds
 * once released */
static HRESULT STDMETHODCALLTYPE cl_close(void *p)
{
    Obj *o = OBJ_AT(p, 1);
    if (o->kind == K_TARGET) {
        lock();
        for (int i = 0; i < MAX_TARGETS; i++) if (g_targets[i] == o) g_targets[i] = 0;
        if (o->hwnd) unsubclass(o->hwnd);
        unlock();
    }
    o->closed = TRUE;
    return S_OK;
}
static const void *const closable_vt[] = { BASE(1), (void *)cl_close };

/* ---- visuals: IVisual (slot 2) ------------------------------------------------------ */
#define V(p) OBJ_AT(p, 2)
#define GETSET(name, field, T) \
    static HRESULT STDMETHODCALLTYPE v_get_##name(void *p, T *out) { if (!out) return E_POINTER; lock(); *out = V(p)->field; unlock(); return S_OK; } \
    static HRESULT STDMETHODCALLTYPE v_put_##name(void *p, T in) { lock(); V(p)->field = in; unlock(); schedule(); return S_OK; }
GETSET(anchor, anchor, Vec2) GETSET(backface, backface, int) GETSET(border, border, int) GETSET(center, center, Vec3)
GETSET(composite, composite, int) GETSET(visible, visible, boolean_t) GETSET(offset, offset, Vec3) GETSET(orient, orient, Quat)
GETSET(angle, angle, float) GETSET(axis, axis, Vec3) GETSET(scale, scale, Vec3) GETSET(size, size, Vec2) GETSET(tm, tm, Mat4)
static HRESULT STDMETHODCALLTYPE v_get_opacity(void *p, float *out) { if (!out) return E_POINTER; *out = V(p)->opacity; return S_OK; }
static HRESULT STDMETHODCALLTYPE v_put_opacity(void *p, float f)
{
    if (!(f >= 0.0f && f <= 1.0f)) return E_INVALIDARG;
    lock(); V(p)->opacity = f; unlock();
    schedule();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE v_get_degrees(void *p, float *out)
{
    if (!out) return E_POINTER;
    *out = V(p)->angle * (180.0f / 3.14159265f);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE v_put_degrees(void *p, float f) { return v_put_angle(p, f * (3.14159265f / 180.0f)); }
static HRESULT STDMETHODCALLTYPE v_get_clip(void *p, void **out) { lock(); HRESULT hr = give(V(p)->clip, 2, out); unlock(); return hr; }
static HRESULT STDMETHODCALLTYPE v_put_clip(void *p, void *clip)
{
    Obj *c = 0;
    if (clip && !(c = obj_of(clip, K_INSET_CLIP, K_RECT_CLIP))) return E_INVALIDARG;
    swap_ref(&V(p)->clip, c);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE v_get_parent(void *p, void **out) { lock(); HRESULT hr = give(V(p)->parent, 3, out); unlock(); return hr; }
static const void *const visual_vt[] = {
    BASE(2),
    (void *)v_get_anchor, (void *)v_put_anchor, (void *)v_get_backface, (void *)v_put_backface, (void *)v_get_border,
    (void *)v_put_border, (void *)v_get_center, (void *)v_put_center, (void *)v_get_clip, (void *)v_put_clip,
    (void *)v_get_composite, (void *)v_put_composite, (void *)v_get_visible, (void *)v_put_visible, (void *)v_get_offset,
    (void *)v_put_offset, (void *)v_get_opacity, (void *)v_put_opacity, (void *)v_get_orient, (void *)v_put_orient,
    (void *)v_get_parent, (void *)v_get_angle, (void *)v_put_angle, (void *)v_get_degrees, (void *)v_put_degrees,
    (void *)v_get_axis, (void *)v_put_axis, (void *)v_get_scale, (void *)v_put_scale, (void *)v_get_size, (void *)v_put_size,
    (void *)v_get_tm, (void *)v_put_tm,
};
_Static_assert(sizeof visual_vt / sizeof visual_vt[0] == 6 + 33, "IVisual has 33 methods");

/* IVisual2 (slot 5): ParentForTransform (kept: drawing uses the parent),
 * RelativeOffsetAdjustment and RelativeSizeAdjustment (fractions of the
 * parent's size); IVisual3 (slot 6): IsHitTestVisible (kept);
 * IVisual4 (slot 7): IsPixelSnappingEnabled (kept) */
static HRESULT STDMETHODCALLTYPE v2_get_pft(void *p, void **out) { lock(); HRESULT hr = give(OBJ_AT(p, 5)->for_transform, 2, out); unlock(); return hr; }
static HRESULT STDMETHODCALLTYPE v2_put_pft(void *p, void *visual)
{
    Obj *v = 0;
    if (visual && !(v = obj_of(visual, K_CONTAINER, K_SPRITE))) return E_INVALIDARG;
    swap_ref(&OBJ_AT(p, 5)->for_transform, v);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE v2_get_rel_offset(void *p, Vec3 *out) { if (!out) return E_POINTER; lock(); *out = OBJ_AT(p, 5)->rel_offset; unlock(); return S_OK; }
static HRESULT STDMETHODCALLTYPE v2_put_rel_offset(void *p, Vec3 in) { lock(); OBJ_AT(p, 5)->rel_offset = in; unlock(); schedule(); return S_OK; }
static HRESULT STDMETHODCALLTYPE v2_get_rel_size(void *p, Vec2 *out) { if (!out) return E_POINTER; lock(); *out = OBJ_AT(p, 5)->rel_size; unlock(); return S_OK; }
static HRESULT STDMETHODCALLTYPE v2_put_rel_size(void *p, Vec2 in) { lock(); OBJ_AT(p, 5)->rel_size = in; unlock(); schedule(); return S_OK; }
static const void *const visual2_vt[] = { BASE(5), (void *)v2_get_pft, (void *)v2_put_pft, (void *)v2_get_rel_offset,
                                          (void *)v2_put_rel_offset, (void *)v2_get_rel_size, (void *)v2_put_rel_size };
static HRESULT STDMETHODCALLTYPE v3_get_hit(void *p, boolean_t *out) { if (!out) return E_POINTER; *out = OBJ_AT(p, 6)->hit_test; return S_OK; }
static HRESULT STDMETHODCALLTYPE v3_put_hit(void *p, boolean_t in) { OBJ_AT(p, 6)->hit_test = in != 0; return S_OK; }
static const void *const visual3_vt[] = { BASE(6), (void *)v3_get_hit, (void *)v3_put_hit };
static HRESULT STDMETHODCALLTYPE v4_get_snap(void *p, boolean_t *out) { if (!out) return E_POINTER; *out = OBJ_AT(p, 7)->snap; return S_OK; }
static HRESULT STDMETHODCALLTYPE v4_put_snap(void *p, boolean_t in) { OBJ_AT(p, 7)->snap = in != 0; return S_OK; }
static const void *const visual4_vt[] = { BASE(7), (void *)v4_get_snap, (void *)v4_put_snap };

/* IContainerVisual (slot 3): Children, a collection on the visual */
static const void *const co_vt[], *const closable_vt[], *const collection_vt[];
static HRESULT STDMETHODCALLTYPE cv_children(void *p, void **out)
{
    Obj *v = OBJ_AT(p, 3);
    if (!out) return E_POINTER;
    *out = 0;
    Obj *c = obj_new(v->comp, K_COLLECTION);
    if (!c) return E_OUTOFMEMORY;
    c->vt[0] = co_vt; c->vt[1] = closable_vt; c->vt[2] = collection_vt;
    c->of = v;
    obj_addref(v);
    *out = IF(c, 2);
    return S_OK;
}
static const void *const container_vt[] = { BASE(3), (void *)cv_children };

/* ISpriteVisual (slot 4): Brush */
static HRESULT STDMETHODCALLTYPE sv_get_brush(void *p, void **out) { lock(); HRESULT hr = give(OBJ_AT(p, 4)->brush, 2, out); unlock(); return hr; }
static HRESULT STDMETHODCALLTYPE sv_put_brush(void *p, void *brush)
{
    Obj *b = 0;
    if (brush && !(b = brush_of(brush))) return E_INVALIDARG;
    swap_ref(&OBJ_AT(p, 4)->brush, b);
    return S_OK;
}
static const void *const sprite_vt[] = { BASE(4), (void *)sv_get_brush, (void *)sv_put_brush };

/* ---- IVisualCollection (slot 2 of a collection) ------------------------------------- */
#define COLL(p) (OBJ_AT(p, 2)->of)
static HRESULT STDMETHODCALLTYPE vc_count(void *p, INT32 *n) { if (!n) return E_POINTER; *n = COLL(p)->nkids; return S_OK; }
static int kid_index(Obj *v, Obj *kid)
{
    for (int i = 0; i < v->nkids; i++) if (v->kids[i] == kid) return i;
    return -1;
}
/* @kid into @v's children at @at (0: the bottom) */
static HRESULT insert(Obj *v, void *child, int where, void *sibling)
{
    Obj *kid = obj_of(child, K_CONTAINER, K_SPRITE), *sib = 0;
    if (!kid) return E_INVALIDARG;
    if (sibling && !(sib = obj_of(sibling, K_CONTAINER, K_SPRITE))) { obj_release(kid); return E_INVALIDARG; }
    HRESULT hr = S_OK;
    lock();
    int at = where == 0 ? 0 : v->nkids, si = sib ? kid_index(v, sib) : 0;
    for (Obj *a = v; a; a = a->parent) if (a == kid) hr = E_INVALIDARG;                /* (into itself) */
    if (kid->parent || kid->target_of || si < 0) hr = E_INVALIDARG;
    if (SUCCEEDED(hr) && sib) at = where == 2 ? si + 1 : si;                         /* (2: above, 3: below) */
    if (SUCCEEDED(hr) && v->nkids == v->capkids) {
        int cap = v->capkids ? v->capkids * 2 : 8;
        Obj **k = v->kids ? HeapReAlloc(GetProcessHeap(), 0, v->kids, cap * sizeof *k) : HeapAlloc(GetProcessHeap(), 0, cap * sizeof *k);
        if (!k) hr = E_OUTOFMEMORY;
        else { v->kids = k; v->capkids = cap; }
    }
    if (SUCCEEDED(hr)) {
        for (int i = v->nkids; i > at; i--) v->kids[i] = v->kids[i - 1];
        v->kids[at] = kid;                                                           /* (the reference obj_of took) */
        v->nkids++;
        kid->parent = v;
        kid = 0;
    }
    unlock();
    if (kid) obj_release(kid);
    if (sib) obj_release(sib);
    schedule();
    return hr;
}
static HRESULT STDMETHODCALLTYPE vc_above(void *p, void *child, void *sib) { return sib ? insert(COLL(p), child, 2, sib) : E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE vc_bottom(void *p, void *child) { return insert(COLL(p), child, 0, 0); }
static HRESULT STDMETHODCALLTYPE vc_top(void *p, void *child) { return insert(COLL(p), child, 1, 0); }
static HRESULT STDMETHODCALLTYPE vc_below(void *p, void *child, void *sib) { return sib ? insert(COLL(p), child, 3, sib) : E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE vc_remove(void *p, void *child)
{
    Obj *v = COLL(p), *kid = obj_of(child, K_CONTAINER, K_SPRITE);
    if (!kid) return E_INVALIDARG;
    lock();
    int i = kid_index(v, kid);
    if (i >= 0) {
        for (; i + 1 < v->nkids; i++) v->kids[i] = v->kids[i + 1];
        v->nkids--;
        kid->parent = 0;
    }
    unlock();
    if (i >= 0) obj_release(kid);                   /* (the collection's) */
    obj_release(kid);
    schedule();
    return i >= 0 ? S_OK : E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE vc_remove_all(void *p)
{
    Obj *v = COLL(p);
    lock();
    Obj **kids = v->kids;
    int n = v->nkids;
    for (int i = 0; i < n; i++) kids[i]->parent = 0;
    v->kids = 0; v->nkids = v->capkids = 0;
    unlock();
    for (int i = 0; i < n; i++) obj_release(kids[i]);
    HeapFree(GetProcessHeap(), 0, kids);
    schedule();
    return S_OK;
}
static const void *const collection_vt[] = { BASE(2), (void *)vc_count, (void *)vc_above, (void *)vc_bottom, (void *)vc_top,
                                             (void *)vc_below, (void *)vc_remove, (void *)vc_remove_all };

/* ---- brushes and clips --------------------------------------------------------------- */
static const void *const empty2_vt[] = { BASE(2) };                 /* ICompositionBrush, ICompositionClip, ICompositionSurface */
static const void *const empty4_vt[] = { BASE(4) };
static HRESULT STDMETHODCALLTYPE cb_get(void *p, Color *c) { if (!c) return E_POINTER; *c = OBJ_AT(p, 3)->color; return S_OK; }
static HRESULT STDMETHODCALLTYPE cb_put(void *p, Color c) { lock(); OBJ_AT(p, 3)->color = c; unlock(); schedule(); return S_OK; }
static const void *const color_vt[] = { BASE(3), (void *)cb_get, (void *)cb_put };

#define SB(p) OBJ_AT(p, 3)
static HRESULT STDMETHODCALLTYPE sb_get_interp(void *p, int *v) { if (!v) return E_POINTER; *v = SB(p)->interp; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb_put_interp(void *p, int v) { SB(p)->interp = v; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb_get_h(void *p, float *v) { if (!v) return E_POINTER; *v = SB(p)->hratio; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb_put_h(void *p, float v) { if (!(v >= 0 && v <= 1)) return E_INVALIDARG; lock(); SB(p)->hratio = v; unlock(); schedule(); return S_OK; }
static HRESULT STDMETHODCALLTYPE sb_get_stretch(void *p, int *v) { if (!v) return E_POINTER; *v = SB(p)->stretch; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb_put_stretch(void *p, int v) { if (v < 0 || v > 3) return E_INVALIDARG; lock(); SB(p)->stretch = v; unlock(); schedule(); return S_OK; }
static int surface_slot(Obj *s) { return s && s->kind == K_DRAWING_SURFACE ? 4 : 2; }
static HRESULT STDMETHODCALLTYPE sb_get_surface(void *p, void **out)
{
    lock();
    Obj *s = SB(p)->surface;
    HRESULT hr = give(s, surface_slot(s), out);
    unlock();
    return hr;
}
static HRESULT STDMETHODCALLTYPE sb_put_surface(void *p, void *surface)
{
    Obj *s = 0;
    if (surface && !(s = obj_of(surface, K_DRAWING_SURFACE, K_CHAIN_SURFACE))) return E_INVALIDARG;
    swap_ref(&SB(p)->surface, s);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sb_get_v(void *p, float *v) { if (!v) return E_POINTER; *v = SB(p)->vratio; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb_put_v(void *p, float v) { if (!(v >= 0 && v <= 1)) return E_INVALIDARG; lock(); SB(p)->vratio = v; unlock(); schedule(); return S_OK; }
static const void *const surface_brush_vt[] = { BASE(3), (void *)sb_get_interp, (void *)sb_put_interp, (void *)sb_get_h, (void *)sb_put_h,
                                                (void *)sb_get_stretch, (void *)sb_put_stretch, (void *)sb_get_surface,
                                                (void *)sb_put_surface, (void *)sb_get_v, (void *)sb_put_v };
/* ICompositionSurfaceBrush2 (slot 4), and a clip's ICompositionClip2
 * (slot 4 too, the same members): the transform of the brush's content or
 * of the clip's shape in the visual: scaled and rotated about CenterPoint,
 * moved by Offset, then TransformMatrix (AnchorPoint is kept, not used) */
#define SB2(p) OBJ_AT(p, 4)
#define SB2_VEC(name, field) \
    static HRESULT STDMETHODCALLTYPE sb2_get_##name(void *p, Vec2 *v) { if (!v) return E_POINTER; *v = SB2(p)->field; return S_OK; } \
    static HRESULT STDMETHODCALLTYPE sb2_put_##name(void *p, Vec2 v) { lock(); SB2(p)->field = v; unlock(); schedule(); return S_OK; }
SB2_VEC(anchor, b_anchor) SB2_VEC(center, b_center) SB2_VEC(offset, b_offset) SB2_VEC(scale, b_scale)
static HRESULT STDMETHODCALLTYPE sb2_get_angle(void *p, float *v) { if (!v) return E_POINTER; *v = SB2(p)->b_angle; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb2_put_angle(void *p, float v) { lock(); SB2(p)->b_angle = v; unlock(); schedule(); return S_OK; }
static HRESULT STDMETHODCALLTYPE sb2_get_deg(void *p, float *v) { if (!v) return E_POINTER; *v = SB2(p)->b_angle * 57.2957795f; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb2_put_deg(void *p, float v) { return sb2_put_angle(p, v / 57.2957795f); }
static HRESULT STDMETHODCALLTYPE sb2_get_tm(void *p, Aff *v) { if (!v) return E_POINTER; *v = SB2(p)->b_tm; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb2_put_tm(void *p, Aff v) { lock(); SB2(p)->b_tm = v; unlock(); schedule(); return S_OK; }
static const void *const surface_brush2_vt[] = { BASE(4), (void *)sb2_get_anchor, (void *)sb2_put_anchor, (void *)sb2_get_center,
                                                 (void *)sb2_put_center, (void *)sb2_get_offset, (void *)sb2_put_offset,
                                                 (void *)sb2_get_angle, (void *)sb2_put_angle, (void *)sb2_get_deg, (void *)sb2_put_deg,
                                                 (void *)sb2_get_scale, (void *)sb2_put_scale, (void *)sb2_get_tm, (void *)sb2_put_tm };
/* ICompositionSurfaceBrush3 (slot 5): SnapToPixels (kept; pixels are sampled nearest) */
static HRESULT STDMETHODCALLTYPE sb3_get_snap(void *p, boolean_t *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 5)->b_snap; return S_OK; }
static HRESULT STDMETHODCALLTYPE sb3_put_snap(void *p, boolean_t v) { OBJ_AT(p, 5)->b_snap = v; return S_OK; }
static const void *const surface_brush3_vt[] = { BASE(5), (void *)sb3_get_snap, (void *)sb3_put_snap };

/* ICompositionMaskBrush (slot 3): Mask, Source (the Source's pixels, each
 * kept as much as the Mask's alpha there says) */
static HRESULT mb_get(Obj *b, void **out) { lock(); HRESULT hr = give(b, b ? default_slot(b) : 0, out); unlock(); return hr; }
static HRESULT STDMETHODCALLTYPE mb_get_mask(void *p, void **out) { return mb_get(OBJ_AT(p, 3)->mask, out); }
static HRESULT STDMETHODCALLTYPE mb_get_source(void *p, void **out) { return mb_get(OBJ_AT(p, 3)->brush, out); }
static HRESULT STDMETHODCALLTYPE mb_put_mask(void *p, void *brush)
{
    Obj *b = 0;
    if (brush && !(b = brush_of(brush))) return E_INVALIDARG;
    swap_ref(&OBJ_AT(p, 3)->mask, b);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE mb_put_source(void *p, void *brush)
{
    Obj *b = 0;
    if (brush && !(b = brush_of(brush))) return E_INVALIDARG;
    swap_ref(&OBJ_AT(p, 3)->brush, b);
    return S_OK;
}
static const void *const mask_brush_vt[] = { BASE(3), (void *)mb_get_mask, (void *)mb_put_mask, (void *)mb_get_source, (void *)mb_put_source };

/* IInsetClip: Bottom, Left, Right, Top */
#define INSET(name, i) \
    static HRESULT STDMETHODCALLTYPE ic_get_##name(void *p, float *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 3)->inset[i]; return S_OK; } \
    static HRESULT STDMETHODCALLTYPE ic_put_##name(void *p, float v) { lock(); OBJ_AT(p, 3)->inset[i] = v; unlock(); schedule(); return S_OK; }
INSET(left, 0) INSET(top, 1) INSET(right, 2) INSET(bottom, 3)
static const void *const inset_vt[] = { BASE(3), (void *)ic_get_bottom, (void *)ic_put_bottom, (void *)ic_get_left, (void *)ic_put_left,
                                        (void *)ic_get_right, (void *)ic_put_right, (void *)ic_get_top, (void *)ic_put_top };

/* ---- targets: ICompositionTarget (slot 2), IDesktopWindowTarget (slot 3) -------------- */
static HRESULT STDMETHODCALLTYPE ct_get_root(void *p, void **out) { lock(); HRESULT hr = give(OBJ_AT(p, 2)->root, 2, out); unlock(); return hr; }
static HRESULT STDMETHODCALLTYPE ct_put_root(void *p, void *visual)
{
    Obj *t = OBJ_AT(p, 2), *v = 0;
    if (visual && !(v = obj_of(visual, K_CONTAINER, K_SPRITE))) return E_INVALIDARG;
    lock();
    if (v && (v->parent || (v->target_of && v->target_of != t))) { unlock(); obj_release(v); return E_INVALIDARG; }
    Obj *old = t->root;
    if (old) old->target_of = 0;
    t->root = v;
    if (v) v->target_of = t;
    unlock();
    if (old) obj_release(old);
    schedule();
    return S_OK;
}
static const void *const target_vt[] = { BASE(2), (void *)ct_get_root, (void *)ct_put_root };
static HRESULT STDMETHODCALLTYPE dt_topmost(void *p, boolean_t *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 3)->topmost; return S_OK; }
static const void *const desktop_target_vt[] = { BASE(3), (void *)dt_topmost };

static HRESULT target_new(Obj *comp, HWND h, BOOL desktop, boolean_t topmost, Obj **out)
{
    *out = 0;
    if (!IsWindow(h)) return E_INVALIDARG;
    Obj *t = obj_new(comp, K_TARGET);
    if (!t) return E_OUTOFMEMORY;
    t->vt[0] = co_vt; t->vt[1] = closable_vt; t->vt[2] = target_vt;
    if (desktop) t->vt[3] = desktop_target_vt;
    t->hwnd = h;
    t->topmost = topmost;
    HRESULT hr = E_OUTOFMEMORY;
    ensure_thread();
    lock();
    for (int i = 0; i < MAX_TARGETS; i++)
        if (!g_targets[i]) { g_targets[i] = t; hr = S_OK; break; }
    if (SUCCEEDED(hr)) subclass(h);
    unlock();
    if (FAILED(hr)) { obj_release(t); return hr; }
    *out = t;
    schedule();
    return S_OK;
}

/* ---- graphics devices and drawing surfaces ---------------------------------------- */
typedef HRESULT (STDMETHODCALLTYPE *CreateTexture2D_t)(IUnknown *, const ND3D11_TEXTURE2D_DESC *, const void *, IUnknown **);
static HRESULT surface_make(Obj *s, UINT w, UINT h)
{
    ND3D11_TEXTURE2D_DESC td;
    __builtin_memset(&td, 0, sizeof(td));
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = s->format;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET_ | D3D11_BIND_SHADER_RESOURCE_;
    IUnknown *dev = s->d3d;
    CreateTexture2D_t create = (CreateTexture2D_t)((void **)*(void **)dev)[5];
    IUnknown *tex = 0, *staging = 0;
    HRESULT hr = create(dev, &td, NULL, &tex);
    if (FAILED(hr)) return hr;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING_;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ_;
    hr = create(dev, &td, NULL, &staging);
    BYTE *frame = SUCCEEDED(hr) ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)w * 4 * h) : 0;
    if (!frame) {
        tex->lpVtbl->Release(tex);
        if (staging) staging->lpVtbl->Release(staging);
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    lock();
    UINT cw = w < s->w ? w : s->w, ch = h < s->h ? h : s->h;
    if (s->tex && cw && ch) {           /* (a resize keeps the pixels both sizes cover) */
        typedef void (STDMETHODCALLTYPE *CopyRegion_t)(IUnknown *, IUnknown *, UINT, UINT, UINT, UINT, IUnknown *, UINT, const ND3D11_BOX *);
        ND3D11_BOX box = { 0, 0, 0, cw, ch, 1 };
        ((CopyRegion_t)((void **)*(void **)s->ctx)[46])(s->ctx, tex, 0, 0, 0, 0, s->tex, 0, &box);
        for (UINT y = 0; s->frame && y < ch; y++) __builtin_memcpy(frame + (size_t)y * w * 4, s->frame + (size_t)y * s->w * 4, (size_t)cw * 4);
    }
    IUnknown *old_tex = s->tex, *old_staging = s->staging;
    BYTE *old_frame = s->frame;
    s->tex = tex; s->staging = staging; s->frame = frame;
    s->w = w; s->h = h;
    unlock();
    if (old_tex) old_tex->lpVtbl->Release(old_tex);
    if (old_staging) old_staging->lpVtbl->Release(old_staging);
    HeapFree(GetProcessHeap(), 0, old_frame);
    schedule();
    return S_OK;
}

static const void *const drawing_vt[], *const drawing_interop_vt[];
/* ICompositionGraphicsDevice (slot 2): CreateDrawingSurface(size in pixels,
 * DirectXPixelFormat (DXGI_FORMAT's values), DirectXAlphaMode) */
static HRESULT STDMETHODCALLTYPE gd_create(void *p, SizeF size, int format, int alpha, void **out)
{
    Obj *g = OBJ_AT(p, 2);
    if (!out) return E_POINTER;
    *out = 0;
    if (!(format == NDXGI_FORMAT_B8G8R8A8_UNORM || format == NDXGI_FORMAT_R8G8B8A8_UNORM || format == NDXGI_FORMAT_B8G8R8X8_UNORM ||
          format == NDXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == NDXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return E_INVALIDARG;
    if (!(size.Width >= 0 && size.Height >= 0)) return E_INVALIDARG;
    if (!g->d3d) return E_ILLEGAL_METHOD_CALL_;
    Obj *s = obj_new(g->comp, K_DRAWING_SURFACE);
    if (!s) return E_OUTOFMEMORY;
    s->vt[0] = co_vt; s->vt[1] = closable_vt; s->vt[2] = drawing_vt; s->vt[3] = drawing_interop_vt; s->vt[4] = empty4_vt;
    s->format = (UINT)format;
    s->alpha = alpha;
    s->d3d = g->d3d; s->d3d->lpVtbl->AddRef(s->d3d);
    s->ctx = g->ctx; s->ctx->lpVtbl->AddRef(s->ctx);
    if (g->d2d) { s->d2d = g->d2d; s->d2d->lpVtbl->AddRef(s->d2d); }
    UINT w = (UINT)(int)size.Width, h = (UINT)(int)size.Height;
    HRESULT hr = w && h ? surface_make(s, w, h) : S_OK;           /* (an empty surface gets its texture at Resize) */
    if (FAILED(hr)) { obj_release(s); return hr; }
    *out = IF(s, 2);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE gd_add_replaced(void *p, void *h, EventRegistrationToken *t)
{
    (void)p; (void)h;
    if (!t) return E_POINTER;
    t->value = 1;                       /* (the device is never replaced) */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE gd_remove_replaced(void *p, EventRegistrationToken t) { (void)p; (void)t; return S_OK; }
static const void *const graphics_vt[] = { BASE(2), (void *)gd_create, (void *)gd_add_replaced, (void *)gd_remove_replaced };

/* @dev (a Direct3D 11 device, or an ID2D1Device on one) as a Direct3D 11
 * device, its immediate context and the ID2D1Device (all counted; the last
 * NULL unless @dev is one) */
static HRESULT d3d_of(IUnknown *dev, IUnknown **d3d, IUnknown **ctx, IUnknown **d2d)
{
    *d3d = *ctx = 0;
    *d3d = novadx_rendering_d3d(dev, d2d);
    if (!*d3d) return E_INVALIDARG;
    ((void (STDMETHODCALLTYPE *)(IUnknown *, IUnknown **))((void **)*(void **)*d3d)[40])(*d3d, ctx);      /* GetImmediateContext */
    if (!*ctx) {
        (*d3d)->lpVtbl->Release(*d3d);
        if (*d2d) (*d2d)->lpVtbl->Release(*d2d);
        *d3d = *d2d = 0;
        return E_FAIL;
    }
    return S_OK;
}
/* ICompositionGraphicsDeviceInterop (slot 3) */
static HRESULT STDMETHODCALLTYPE gdi_get(void *p, IUnknown **out)
{
    Obj *g = OBJ_AT(p, 3);
    if (!out) return E_POINTER;
    *out = g->d2d ? g->d2d : g->d3d;
    if (*out) (*out)->lpVtbl->AddRef(*out);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE gdi_set(void *p, IUnknown *dev)
{
    Obj *g = OBJ_AT(p, 3);
    IUnknown *d3d, *ctx, *d2d;
    HRESULT hr = d3d_of(dev, &d3d, &ctx, &d2d);
    if (FAILED(hr)) return hr;
    if (g->d3d) g->d3d->lpVtbl->Release(g->d3d);
    if (g->ctx) g->ctx->lpVtbl->Release(g->ctx);
    if (g->d2d) g->d2d->lpVtbl->Release(g->d2d);
    g->d3d = d3d; g->ctx = ctx; g->d2d = d2d;
    return S_OK;
}
static const void *const graphics_interop_vt[] = { UNK(3), (void *)gdi_get, (void *)gdi_set };

/* ICompositionDrawingSurface (slot 2) */
static HRESULT STDMETHODCALLTYPE ds_alpha(void *p, int *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 2)->alpha; return S_OK; }
static HRESULT STDMETHODCALLTYPE ds_format(void *p, int *v) { if (!v) return E_POINTER; *v = (int)OBJ_AT(p, 2)->format; return S_OK; }
static HRESULT STDMETHODCALLTYPE ds_size(void *p, SizeF *v) { if (!v) return E_POINTER; v->Width = (float)OBJ_AT(p, 2)->w; v->Height = (float)OBJ_AT(p, 2)->h; return S_OK; }
static const void *const drawing_vt[] = { BASE(2), (void *)ds_alpha, (void *)ds_format, (void *)ds_size };

/* ICompositionDrawingSurfaceInterop (slot 3): BeginDraw hands out the
 * surface's texture (as @iid: ID3D11Texture2D, IDXGISurface) with the
 * update rectangle at *@offset in it, or on a graphics device made on an
 * ID2D1Device an ID2D1DeviceContext drawing into it; EndDraw reads the
 * rectangle back */
#define DS(p) OBJ_AT(p, 3)
static HRESULT STDMETHODCALLTYPE dsi_begin(void *p, const RECT *r, REFIID iid, void **out, POINT *offset)
{
    Obj *s = DS(p);
    if (!out || !offset) return E_POINTER;
    *out = 0;
    if (s->drawing) return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED_;
    if (!s->tex) return E_ILLEGAL_METHOD_CALL_;
    RECT all = { 0, 0, (LONG)s->w, (LONG)s->h }, u = all;
    if (r) {
        if (r->left < 0 || r->top < 0 || r->right > all.right || r->bottom > all.bottom || r->left >= r->right || r->top >= r->bottom)
            return E_INVALIDARG;
        u = *r;
    }
    HRESULT hr;
    if (novadx_same(iid, &NIID_ID2D1DeviceContext)) {
        hr = novadx_d2d_begin(s->d2d, s->tex, &s->dc);
        if (FAILED(hr)) return hr;
        s->dc->lpVtbl->AddRef(s->dc);
        *out = s->dc;
    } else {
        hr = s->tex->lpVtbl->QueryInterface(s->tex, iid, out);
        if (FAILED(hr)) return hr;
    }
    s->drawing = TRUE;
    s->update = u;
    offset->x = u.left; offset->y = u.top;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsi_end(void *p)
{
    Obj *s = DS(p);
    if (!s->drawing) return DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED_;
    if (s->dc) { novadx_d2d_end(s->dc); s->dc = NULL; }
    lock();
    novadx_read_back(s->ctx, s->staging, s->tex, s->format, s->w, s->h, s->frame, &s->update);
    unlock();
    s->drawing = FALSE;
    schedule();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsi_resize(void *p, SIZE size)
{
    Obj *s = DS(p);
    if (s->drawing) return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED_;
    if (size.cx <= 0 || size.cy <= 0) return E_INVALIDARG;
    if ((UINT)size.cx == s->w && (UINT)size.cy == s->h && s->tex) return S_OK;
    return surface_make(s, (UINT)size.cx, (UINT)size.cy);
}
static HRESULT STDMETHODCALLTYPE dsi_scroll(void *p, const RECT *scroll, const RECT *clip, int dx, int dy)
{ (void)p; (void)scroll; (void)clip; (void)dx; (void)dy; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE dsi_resume(void *p) { return DS(p)->drawing ? S_OK : DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED_; }
static const void *const drawing_interop_vt[] = { UNK(3), (void *)dsi_begin, (void *)dsi_end, (void *)dsi_resize, (void *)dsi_scroll,
                                                  (void *)dsi_resume, (void *)dsi_resume };

/* a present of a swap chain a surface shows */
static void STDMETHODCALLTYPE chain_sink(void *ctx, const RECT *dirty) { (void)ctx; (void)dirty; schedule(); }

/* ---- values and property sets ---------------------------------------------------
 * Property sets, animations' parameters and key frames hold values as up
 * to 16 floats: a colour as A, R, G, B (0-255), a Boolean as 0 or 1 */
typedef struct { float X, Y, Z, W; } Vec4;
typedef struct { float m[6]; } Mat3x2;
static void val_in(int type, const void *src, float *f)
{
    if (type == P_COLOR) { const Color *c = src; f[0] = c->A; f[1] = c->R; f[2] = c->G; f[3] = c->B; }
    else if (type == P_BOOL) f[0] = *(const boolean_t *)src ? 1.0f : 0.0f;
    else __builtin_memcpy(f, src, p_count[type] * sizeof(float));
}
static BYTE to_byte(float f) { return f <= 0 ? 0 : f >= 255 ? 255 : (BYTE)(int)(f + 0.5f); }
static void val_out(int type, const float *f, void *dst)
{
    if (type == P_COLOR) { Color *c = dst; c->A = to_byte(f[0]); c->R = to_byte(f[1]); c->G = to_byte(f[2]); c->B = to_byte(f[3]); }
    else if (type == P_BOOL) *(boolean_t *)dst = f[0] != 0.0f;
    else __builtin_memcpy(dst, f, p_count[type] * sizeof(float));
}
static WCHAR *wdup(const WCHAR *s, UINT32 n)
{
    WCHAR *d = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
    if (!d) return 0;
    if (n) __builtin_memcpy(d, s, n * sizeof(WCHAR));
    d[n] = 0;
    return d;
}
static const WCHAR *hbuf(HSTRING s, UINT32 *n)
{
    const WCHAR *c = WindowsGetStringRawBuffer(s, n);
    return c ? c : L"";
}
/* @a (NUL-terminated) is @b's @n characters */
static BOOL wsame(const WCHAR *a, const WCHAR *b, UINT32 n)
{
    for (UINT32 i = 0; i < n; i++) if (a[i] != b[i]) return FALSE;
    return !a[n];
}
/* (each list grows one item at a time, under the lock) */
static BOOL grow(void **items, int n, size_t size)
{
    void *v = *items ? HeapReAlloc(GetProcessHeap(), 0, *items, (n + 1) * size) : HeapAlloc(GetProcessHeap(), 0, size);
    if (!v) return FALSE;
    *items = v;
    return TRUE;
}

static Prop *prop_find(Obj *o, const WCHAR *name, UINT32 n)
{
    for (int i = 0; i < o->nprops; i++) if (wsame(o->propv[i].name, name, n)) return &o->propv[i];
    return 0;
}
/* Insert*: a new property, or a new value of one of the same type */
static HRESULT prop_insert(Obj *o, HSTRING name, int type, const void *value)
{
    UINT32 n;
    const WCHAR *c = hbuf(name, &n);
    if (!n) return E_INVALIDARG;
    HRESULT hr = S_OK;
    lock();
    Prop *p = prop_find(o, c, n);
    if (p && p->type != type) hr = E_INVALIDARG;
    else if (!p) {
        WCHAR *nm = wdup(c, n);
        if (!nm || !grow((void **)&o->propv, o->nprops, sizeof *o->propv)) { HeapFree(GetProcessHeap(), 0, nm); hr = E_OUTOFMEMORY; }
        else { p = &o->propv[o->nprops++]; p->name = nm; p->type = type; }
    }
    if (SUCCEEDED(hr)) val_in(type, value, p->v);
    unlock();
    schedule();
    return hr;
}
/* TryGet*: *@status a CompositionGetValueStatus (Succeeded 0, TypeMismatch 1, NotFound 2) */
static HRESULT prop_get(Obj *o, HSTRING name, int type, void *value, int *status)
{
    if (!value || !status) return E_POINTER;
    UINT32 n;
    const WCHAR *c = hbuf(name, &n);
    float f[16] = { 0 };
    lock();
    Prop *p = prop_find(o, c, n);
    *status = !p ? 2 : p->type != type ? 1 : 0;
    if (!*status) __builtin_memcpy(f, p->v, sizeof f);
    unlock();
    val_out(type, f, value);
    return S_OK;
}
#define PROPS(name, T, type, slot) \
    static HRESULT STDMETHODCALLTYPE ps_insert_##name(void *p, HSTRING n, T v) { return prop_insert(OBJ_AT(p, slot), n, type, &v); } \
    static HRESULT STDMETHODCALLTYPE ps_get_##name(void *p, HSTRING n, T *v, int *st) { return prop_get(OBJ_AT(p, slot), n, type, v, st); }
PROPS(color, Color, P_COLOR, 2) PROPS(m3x2, Mat3x2, P_M3X2, 2) PROPS(m4x4, Mat4, P_M4X4, 2) PROPS(quat, Quat, P_QUAT, 2)
PROPS(scalar, float, P_SCALAR, 2) PROPS(vec2, Vec2, P_VEC2, 2) PROPS(vec3, Vec3, P_VEC3, 2) PROPS(vec4, Vec4, P_VEC4, 2)
PROPS(bool, boolean_t, P_BOOL, 3)
/* ICompositionPropertySet (slot 2), ICompositionPropertySet2 (slot 3) */
static const void *const propset_vt[] = {
    BASE(2), (void *)ps_insert_color, (void *)ps_insert_m3x2, (void *)ps_insert_m4x4, (void *)ps_insert_quat,
    (void *)ps_insert_scalar, (void *)ps_insert_vec2, (void *)ps_insert_vec3, (void *)ps_insert_vec4,
    (void *)ps_get_color, (void *)ps_get_m3x2, (void *)ps_get_m4x4, (void *)ps_get_quat,
    (void *)ps_get_scalar, (void *)ps_get_vec2, (void *)ps_get_vec3, (void *)ps_get_vec4,
};
static const void *const propset2_vt[] = { BASE(3), (void *)ps_insert_bool, (void *)ps_get_bool };
static Obj *propset_new(Obj *comp)
{
    Obj *o = obj_new(comp, K_PROPSET);
    if (o) { o->vt[2] = propset_vt; o->vt[3] = propset2_vt; }
    return o;
}
/* an object's Properties: made when first asked for (a property set's are its own) */
static HRESULT obj_properties(Obj *o, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    if (o->kind == K_PROPSET) return give(o, 2, out);
    lock();
    if (!o->props) o->props = propset_new(o->comp);
    HRESULT hr = o->props ? give(o->props, 2, out) : E_OUTOFMEMORY;
    unlock();
    return hr;
}

/* ---- easing functions ------------------------------------------------------------
 * Linear, cubic Bezier and step easing, and the sine, circle and power
 * curves (CompositionEasingFunctionMode In 0, Out 1, InOut 2; a power's
 * exponent rounded to a whole number); back, bounce, elastic and
 * exponential ease linearly here */
static float bezier(float a, float b, float s) { float u = 1 - s; return 3 * u * u * s * a + 3 * u * s * s * b + s * s * s; }
static float ease_in(const Ease *e, float t)
{
    switch (e->type) {
    case E_SINE: return 1 - (float)c_cos(t * 3.14159265358979f / 2);
    case E_CIRCLE: return 1 - c_sqrt(1 - t * t);
    case E_POWER: {
        int k = (int)(e->a + 0.5f);
        float r = 1;
        for (int i = 0; i < k; i++) r *= t;
        return k < 1 ? t : r;
    }
    }
    return t;
}
static float ease_at(const Ease *e, float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (e->type) {
    case E_CUBIC: {                     /* (x(s) = t found by halving, then y(s)) */
        float lo = 0, hi = 1, s = t;
        for (int i = 0; i < 24; i++) {
            s = (lo + hi) / 2;
            if (bezier(e->x1, e->x2, s) < t) lo = s; else hi = s;
        }
        return bezier(e->y1, e->y2, s);
    }
    case E_STEP: {
        int n = e->steps > 0 ? e->steps : 1, k = (int)(t * n);
        if (k < e->first) k = e->first;
        if (k > e->last) k = e->last;
        return (float)k / n;
    }
    case E_SINE: case E_CIRCLE: case E_POWER:
        if (e->mode == 1) return 1 - ease_in(e, 1 - t);
        if (e->mode == 2) return t < 0.5f ? ease_in(e, 2 * t) / 2 : 1 - ease_in(e, 2 - 2 * t) / 2;
        return ease_in(e, t);
    }
    return t;
}
static const void *const empty3_vt[] = { BASE(3) };
/* ICubicBezierEasingFunction (slot 3): ControlPoint1, ControlPoint2 */
static HRESULT STDMETHODCALLTYPE cbe_p1(void *p, Vec2 *v) { if (!v) return E_POINTER; v->X = OBJ_AT(p, 3)->ease.x1; v->Y = OBJ_AT(p, 3)->ease.y1; return S_OK; }
static HRESULT STDMETHODCALLTYPE cbe_p2(void *p, Vec2 *v) { if (!v) return E_POINTER; v->X = OBJ_AT(p, 3)->ease.x2; v->Y = OBJ_AT(p, 3)->ease.y2; return S_OK; }
static const void *const cubic_vt[] = { BASE(3), (void *)cbe_p1, (void *)cbe_p2 };
/* IStepEasingFunction (slot 3): FinalStep, InitialStep, IsFinalStepSingleFrame,
 * IsInitialStepSingleFrame (kept), StepCount */
#define STEP(name, field, T) \
    static HRESULT STDMETHODCALLTYPE se_get_##name(void *p, T *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 3)->ease.field; return S_OK; } \
    static HRESULT STDMETHODCALLTYPE se_put_##name(void *p, T v) { OBJ_AT(p, 3)->ease.field = v; return S_OK; }
STEP(final, last, int) STEP(initial, first, int) STEP(final_single, last_single, boolean_t) STEP(initial_single, first_single, boolean_t)
static HRESULT STDMETHODCALLTYPE se_get_count(void *p, int *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 3)->ease.steps; return S_OK; }
static HRESULT STDMETHODCALLTYPE se_put_count(void *p, int v)
{
    Ease *e = &OBJ_AT(p, 3)->ease;
    if (v < 1) return E_INVALIDARG;
    if (e->last == e->steps) e->last = v;           /* (FinalStep follows the count until set) */
    e->steps = v;
    return S_OK;
}
static const void *const step_vt[] = { BASE(3), (void *)se_get_final, (void *)se_put_final, (void *)se_get_initial, (void *)se_put_initial,
                                        (void *)se_get_final_single, (void *)se_put_final_single, (void *)se_get_initial_single,
                                        (void *)se_put_initial_single, (void *)se_get_count, (void *)se_put_count };
static HRESULT easing_new(Obj *comp, Ease e, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *o = obj_new(comp, K_EASING);
    if (!o) return E_OUTOFMEMORY;
    o->ease = e;
    o->vt[2] = empty2_vt;
    o->vt[3] = e.type == E_LINEAR ? empty3_vt : e.type == E_CUBIC ? cubic_vt : e.type == E_STEP ? step_vt : 0;
    *out = IF(o, default_slot(o));
    return S_OK;
}
static HRESULT easing_cubic(Obj *comp, Vec2 a, Vec2 b, void **out)
{
    if (!(a.X >= 0 && a.X <= 1 && b.X >= 0 && b.X <= 1)) { if (out) *out = 0; return E_INVALIDARG; }
    Ease e = { E_CUBIC, 0, a.X, a.Y, b.X, b.Y, 0, 0, 0, 0, 0, 0 };
    return easing_new(comp, e, out);
}
static HRESULT easing_step(Obj *comp, int steps, void **out)
{
    if (steps < 1) { if (out) *out = 0; return E_INVALIDARG; }
    Ease e = { E_STEP, 0, 0, 0, 0, 0, 0, steps, 0, steps, 0, 0 };
    return easing_new(comp, e, out);
}
static HRESULT easing_kind(Obj *comp, int type, int mode, float a, void **out)
{
    Ease e = { type, mode, 0, 0, 0, 0, a, 0, 0, 0, 0, 0 };
    return easing_new(comp, e, out);
}

/* ---- animations -----------------------------------------------------------------
 * ExpressionAnimation (its expression kept, not evaluated) and the key
 * frame animations of scalars, vectors, colours, quaternions and Booleans
 * (values, eased; expression key frames "this.StartingValue",
 * "this.FinalValue" and numbers), their parameters (kept), Target, delay,
 * duration, iterations, direction and stop and delay behaviours */
static HRESULT param_set(Obj *a, HSTRING name, int type, const void *v, Obj *ref)
{
    UINT32 n;
    const WCHAR *c = hbuf(name, &n);
    if (!n) { if (ref) obj_release(ref); return E_INVALIDARG; }
    HRESULT hr = S_OK;
    Obj *old = 0;
    lock();
    Param *p = 0;
    for (int i = 0; i < a->nparams && !p; i++) if (wsame(a->params[i].name, c, n)) p = &a->params[i];
    if (!p) {
        WCHAR *nm = wdup(c, n);
        if (!nm || !grow((void **)&a->params, a->nparams, sizeof *a->params)) { HeapFree(GetProcessHeap(), 0, nm); hr = E_OUTOFMEMORY; }
        else { p = &a->params[a->nparams++]; p->name = nm; p->ref = 0; }
    }
    if (p) {
        old = p->ref;
        p->ref = ref;
        p->type = type;
        if (v) val_in(type, v, p->v);
        ref = 0;
    }
    unlock();
    if (old) obj_release(old);
    if (ref) obj_release(ref);
    return hr;
}
#define AP(name, T, type, slot) \
    static HRESULT STDMETHODCALLTYPE an_##name(void *p, HSTRING n, T v) { return param_set(OBJ_AT(p, slot), n, type, &v, 0); }
AP(color, Color, P_COLOR, 2) AP(m3x2, Mat3x2, P_M3X2, 2) AP(m4x4, Mat4, P_M4X4, 2) AP(quat, Quat, P_QUAT, 2) AP(scalar, float, P_SCALAR, 2)
AP(vec2, Vec2, P_VEC2, 2) AP(vec3, Vec3, P_VEC3, 2) AP(vec4, Vec4, P_VEC4, 2) AP(bool, boolean_t, P_BOOL, 3)
static HRESULT STDMETHODCALLTYPE an_ref(void *p, HSTRING n, void *obj)
{
    Obj *r = obj_of(obj, -1, -1);
    if (!r) return E_INVALIDARG;
    return param_set(OBJ_AT(p, 2), n, P_NONE, 0, r);
}
static HRESULT STDMETHODCALLTYPE an_clear(void *p, HSTRING n)
{
    Obj *a = OBJ_AT(p, 2), *ref = 0;
    UINT32 len;
    const WCHAR *c = hbuf(n, &len);
    lock();
    for (int i = 0; i < a->nparams; i++)
        if (wsame(a->params[i].name, c, len)) {
            HeapFree(GetProcessHeap(), 0, a->params[i].name);
            ref = a->params[i].ref;
            a->params[i] = a->params[--a->nparams];
            break;
        }
    unlock();
    if (ref) obj_release(ref);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE an_clear_all(void *p)
{
    Obj *a = OBJ_AT(p, 2);
    lock();
    Param *v = a->params;
    int n = a->nparams;
    a->params = 0; a->nparams = 0;
    unlock();
    for (int i = 0; i < n; i++) {
        HeapFree(GetProcessHeap(), 0, v[i].name);
        if (v[i].ref) obj_release(v[i].ref);
    }
    HeapFree(GetProcessHeap(), 0, v);
    return S_OK;
}
/* ICompositionAnimation (slot 2) */
static const void *const anim_vt[] = { BASE(2), (void *)an_clear_all, (void *)an_clear, (void *)an_color, (void *)an_m3x2, (void *)an_m4x4,
                                       (void *)an_quat, (void *)an_ref, (void *)an_scalar, (void *)an_vec2, (void *)an_vec3, (void *)an_vec4 };
/* ICompositionAnimation2 (slot 3): SetBooleanParameter, Target */
static HRESULT STDMETHODCALLTYPE an_get_target(void *p, HSTRING *out)
{
    Obj *a = OBJ_AT(p, 3);
    if (!out) return E_POINTER;
    lock();
    HRESULT hr = a->target_name ? make_hstring(a->target_name, out) : (*out = 0, S_OK);
    unlock();
    return hr;
}
static HRESULT STDMETHODCALLTYPE an_put_target(void *p, HSTRING s)
{
    Obj *a = OBJ_AT(p, 3);
    UINT32 n;
    const WCHAR *c = hbuf(s, &n);
    WCHAR *nm = n ? wdup(c, n) : 0;
    if (n && !nm) return E_OUTOFMEMORY;
    lock();
    WCHAR *old = a->target_name;
    a->target_name = nm;
    unlock();
    HeapFree(GetProcessHeap(), 0, old);
    return S_OK;
}
static const void *const anim2_vt[] = { BASE(3), (void *)an_bool, (void *)an_get_target, (void *)an_put_target };
/* ICompositionAnimation3 (slot 5): InitialValueExpressions (not here);
 * ICompositionAnimation4 (slot 6): SetExpressionReferenceParameter (ours kept) */
static HRESULT STDMETHODCALLTYPE an_initial(void *p, void **out) { (void)p; if (out) *out = 0; return E_NOTIMPL; }
static const void *const anim3_vt[] = { BASE(5), (void *)an_initial };
static HRESULT STDMETHODCALLTYPE an_expr_ref(void *p, HSTRING n, void *obj)
{
    Obj *r = obj_of(obj, -1, -1);
    return r ? param_set(OBJ_AT(p, 6), n, P_NONE, 0, r) : (obj ? S_OK : E_INVALIDARG);
}
static const void *const anim4_vt[] = { BASE(6), (void *)an_expr_ref };
/* IExpressionAnimation (slot 7): Expression */
static HRESULT STDMETHODCALLTYPE ea_get(void *p, HSTRING *out)
{
    Obj *a = OBJ_AT(p, 7);
    if (!out) return E_POINTER;
    lock();
    HRESULT hr = make_hstring(a->expr ? a->expr : L"", out);
    unlock();
    return hr;
}
static HRESULT STDMETHODCALLTYPE ea_put(void *p, HSTRING s)
{
    Obj *a = OBJ_AT(p, 7);
    UINT32 n;
    const WCHAR *c = hbuf(s, &n);
    WCHAR *e = wdup(c, n);
    if (!e) return E_OUTOFMEMORY;
    lock();
    WCHAR *old = a->expr;
    a->expr = e;
    unlock();
    HeapFree(GetProcessHeap(), 0, old);
    return S_OK;
}
static const void *const expression_vt[] = { BASE(7), (void *)ea_get, (void *)ea_put };

/* a key frame at @t (0-1; one already at @t is replaced) */
static HRESULT kf_insert(Obj *a, float t, int kind, const float *v, void *easing)
{
    if (!(t >= 0.0f && t <= 1.0f)) return E_INVALIDARG;
    KF k;
    __builtin_memset(&k, 0, sizeof k);
    k.t = t;
    k.kind = kind;
    if (v) __builtin_memcpy(k.v, v, sizeof k.v);
    if (easing) {
        Obj *e = obj_of(easing, K_EASING, -1);
        if (!e) return E_INVALIDARG;
        k.ease = e->ease;
        obj_release(e);
    }
    HRESULT hr = S_OK;
    lock();
    int i = 0;
    while (i < a->nkfs && a->kfs[i].t < t) i++;
    if (i < a->nkfs && a->kfs[i].t == t) a->kfs[i] = k;
    else if (!grow((void **)&a->kfs, a->nkfs, sizeof *a->kfs)) hr = E_OUTOFMEMORY;
    else {
        for (int j = a->nkfs; j > i; j--) a->kfs[j] = a->kfs[j - 1];
        a->kfs[i] = k;
        a->nkfs++;
    }
    unlock();
    return hr;
}
/* an expression key frame: what is understood of it */
static int expr_kind(HSTRING s, float *v)
{
    UINT32 n;
    const WCHAR *c = hbuf(s, &n);
    while (n && *c == ' ') { c++; n--; }
    while (n && c[n - 1] == ' ') n--;
    if (wsame(L"this.StartingValue", c, n)) return KF_START;
    if (wsame(L"this.FinalValue", c, n)) return KF_FINAL;
    UINT32 i = 0;
    float sign = 1, f = 0, scale = 0;
    if (i < n && (c[i] == '-' || c[i] == '+')) sign = c[i++] == '-' ? -1.0f : 1.0f;
    BOOL digits = FALSE;
    for (; i < n; i++) {
        if (c[i] >= '0' && c[i] <= '9') {
            digits = TRUE;
            if (scale) { f += (c[i] - '0') * scale; scale /= 10; }
            else f = f * 10 + (c[i] - '0');
        } else if (c[i] == '.' && !scale) scale = 0.1f;
        else if ((c[i] == 'f' || c[i] == 'F') && i + 1 == n) ;
        else return KF_EXPR;
    }
    if (!digits) return KF_EXPR;
    for (int k = 0; k < 16; k++) v[k] = sign * f;
    return KF_VALUE;
}
/* IKeyFrameAnimation (slot 7) */
#define KA(p) OBJ_AT(p, 7)
#define KGETSET(name, field, T, check) \
    static HRESULT STDMETHODCALLTYPE ka_get_##name(void *p, T *v) { if (!v) return E_POINTER; *v = KA(p)->field; return S_OK; } \
    static HRESULT STDMETHODCALLTYPE ka_put_##name(void *p, T v) { if (!(check)) return E_INVALIDARG; KA(p)->field = v; return S_OK; }
KGETSET(delay, delay, INT64, v >= 0) KGETSET(duration, duration, INT64, v >= 10000) KGETSET(iter_behavior, iter_behavior, int, v == 0 || v == 1)
KGETSET(iter_count, iter_count, int, v >= 1) KGETSET(stop_behavior, stop_behavior, int, v >= 0 && v <= 2)
static HRESULT STDMETHODCALLTYPE ka_count(void *p, int *v) { if (!v) return E_POINTER; lock(); *v = KA(p)->nkfs; unlock(); return S_OK; }
static HRESULT STDMETHODCALLTYPE ka_expr(void *p, float t, HSTRING s) { float v[16] = { 0 }; int k = expr_kind(s, v); return kf_insert(KA(p), t, k, v, 0); }
static HRESULT STDMETHODCALLTYPE ka_expr_e(void *p, float t, HSTRING s, void *e) { float v[16] = { 0 }; int k = expr_kind(s, v); return kf_insert(KA(p), t, k, v, e); }
static const void *const keyframe_vt[] = {
    BASE(7), (void *)ka_get_delay, (void *)ka_put_delay, (void *)ka_get_duration, (void *)ka_put_duration,
    (void *)ka_get_iter_behavior, (void *)ka_put_iter_behavior, (void *)ka_get_iter_count, (void *)ka_put_iter_count,
    (void *)ka_count, (void *)ka_get_stop_behavior, (void *)ka_put_stop_behavior, (void *)ka_expr, (void *)ka_expr_e,
};
/* IKeyFrameAnimation2 (slot 8): Direction; IKeyFrameAnimation3 (slot 9): DelayBehavior */
static HRESULT STDMETHODCALLTYPE ka_get_dir(void *p, int *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 8)->direction; return S_OK; }
static HRESULT STDMETHODCALLTYPE ka_put_dir(void *p, int v) { if (v < 0 || v > 3) return E_INVALIDARG; OBJ_AT(p, 8)->direction = v; return S_OK; }
static const void *const keyframe2_vt[] = { BASE(8), (void *)ka_get_dir, (void *)ka_put_dir };
static HRESULT STDMETHODCALLTYPE ka_get_db(void *p, int *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 9)->delay_behavior; return S_OK; }
static HRESULT STDMETHODCALLTYPE ka_put_db(void *p, int v) { if (v < 0 || v > 1) return E_INVALIDARG; OBJ_AT(p, 9)->delay_behavior = v; return S_OK; }
static const void *const keyframe3_vt[] = { BASE(9), (void *)ka_get_db, (void *)ka_put_db };
/* the typed key frame interfaces (slot 10): InsertKeyFrame(progress, value[, easing]) */
#define KINS(name, T, type) \
    static HRESULT STDMETHODCALLTYPE kf_##name(void *p, float t, T v) { float f[16] = { 0 }; val_in(type, &v, f); return kf_insert(OBJ_AT(p, 10), t, KF_VALUE, f, 0); } \
    static HRESULT STDMETHODCALLTYPE kf_##name##_e(void *p, float t, T v, void *e) { float f[16] = { 0 }; val_in(type, &v, f); return kf_insert(OBJ_AT(p, 10), t, KF_VALUE, f, e); }
KINS(scalar, float, P_SCALAR) KINS(vec2, Vec2, P_VEC2) KINS(vec3, Vec3, P_VEC3) KINS(vec4, Vec4, P_VEC4) KINS(quat, Quat, P_QUAT)
KINS(color, Color, P_COLOR)
static HRESULT STDMETHODCALLTYPE kf_bool(void *p, float t, boolean_t v) { float f[16] = { v ? 1.0f : 0.0f }; return kf_insert(OBJ_AT(p, 10), t, KF_VALUE, f, 0); }
/* (a colour's interpolation colour space is kept; colours blend in RGB) */
static HRESULT STDMETHODCALLTYPE kf_get_space(void *p, int *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 10)->color_space; return S_OK; }
static HRESULT STDMETHODCALLTYPE kf_put_space(void *p, int v) { OBJ_AT(p, 10)->color_space = v; return S_OK; }
static const void *const kf_scalar_vt[] = { BASE(10), (void *)kf_scalar, (void *)kf_scalar_e };
static const void *const kf_vec2_vt[] = { BASE(10), (void *)kf_vec2, (void *)kf_vec2_e };
static const void *const kf_vec3_vt[] = { BASE(10), (void *)kf_vec3, (void *)kf_vec3_e };
static const void *const kf_vec4_vt[] = { BASE(10), (void *)kf_vec4, (void *)kf_vec4_e };
static const void *const kf_quat_vt[] = { BASE(10), (void *)kf_quat, (void *)kf_quat_e };
static const void *const kf_color_vt[] = { BASE(10), (void *)kf_get_space, (void *)kf_put_space, (void *)kf_color, (void *)kf_color_e };
static const void *const kf_bool_vt[] = { BASE(10), (void *)kf_bool };
static const void *const *const kf_typed_vt[] = { [P_SCALAR] = kf_scalar_vt, [P_VEC2] = kf_vec2_vt, [P_VEC3] = kf_vec3_vt,
                                                  [P_VEC4] = kf_vec4_vt, [P_COLOR] = kf_color_vt, [P_QUAT] = kf_quat_vt,
                                                  [P_M3X2] = 0, [P_M4X4] = 0, [P_BOOL] = kf_bool_vt };
static HRESULT anim_new(Obj *comp, int kind, int type, HSTRING expr, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *a = obj_new(comp, kind);
    if (!a) return E_OUTOFMEMORY;
    a->atype = type;
    a->vt[2] = anim_vt; a->vt[3] = anim2_vt; a->vt[4] = empty4_vt; a->vt[5] = anim3_vt; a->vt[6] = anim4_vt;
    if (kind == K_EXPRESSION) {
        a->vt[7] = expression_vt;
        UINT32 n;
        const WCHAR *c = hbuf(expr, &n);
        if (!(a->expr = wdup(c, n))) { obj_release(a); return E_OUTOFMEMORY; }
    } else {
        a->vt[7] = keyframe_vt; a->vt[8] = keyframe2_vt; a->vt[9] = keyframe3_vt; a->vt[10] = kf_typed_vt[type];
        a->duration = 10000000;         /* (one second) */
        a->iter_count = 1;
    }
    *out = IF(a, default_slot(a));
    return S_OK;
}

/* ICompositionAnimationGroup (slot 2): Count, Add, Remove, RemoveAll (its
 * animations are the group's kids) */
#define GRP(p) OBJ_AT(p, 2)
static HRESULT STDMETHODCALLTYPE ag_count(void *p, INT32 *n) { if (!n) return E_POINTER; lock(); *n = GRP(p)->nkids; unlock(); return S_OK; }
static HRESULT STDMETHODCALLTYPE ag_add(void *p, void *anim)
{
    Obj *g = GRP(p), *a = obj_of(anim, K_EXPRESSION, K_KEYFRAME);
    if (!a) return E_INVALIDARG;
    lock();
    BOOL ok = grow((void **)&g->kids, g->nkids, sizeof *g->kids);
    if (ok) g->kids[g->nkids++] = a;
    unlock();
    if (!ok) obj_release(a);
    return ok ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE ag_remove(void *p, void *anim)
{
    Obj *g = GRP(p), *a = obj_of(anim, K_EXPRESSION, K_KEYFRAME), *gone = 0;
    if (!a) return E_INVALIDARG;
    lock();
    for (int i = 0; i < g->nkids; i++)
        if (g->kids[i] == a) {
            gone = a;
            for (; i + 1 < g->nkids; i++) g->kids[i] = g->kids[i + 1];
            g->nkids--;
            break;
        }
    unlock();
    if (gone) obj_release(gone);
    obj_release(a);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ag_remove_all(void *p)
{
    Obj *g = GRP(p);
    lock();
    Obj **k = g->kids;
    int n = g->nkids;
    g->kids = 0; g->nkids = g->capkids = 0;
    unlock();
    for (int i = 0; i < n; i++) obj_release(k[i]);
    HeapFree(GetProcessHeap(), 0, k);
    return S_OK;
}
static const void *const group_vt[] = { BASE(2), (void *)ag_count, (void *)ag_add, (void *)ag_remove, (void *)ag_remove_all };

/* ---- running animations ---------------------------------------------------------
 * StartAnimation binds an animation to a property of the object by name:
 * a visual's (AnchorPoint, CenterPoint, Offset, Opacity, Orientation,
 * RotationAngle, RotationAngleInDegrees, RotationAxis, Scale, Size,
 * TransformMatrix, RelativeOffsetAdjustment, RelativeSizeAdjustment,
 * IsVisible), a colour brush's Color, an inset clip's insets, a rectangle
 * clip's sides, or a property of its property set (the object's
 * Properties), with ".X" ".Y" ".Z" ".W" naming one component.  A key frame
 * animation's key frames are copied, and the compositor's thread moves it
 * on in time, writing the property; an expression animation is held on its
 * property until stopped, not evaluated.  An animation started while a
 * scoped batch is open counts in it. */
typedef struct { float *f; int n, type; float k; Color *color; boolean_t *b; } Bind;
static const struct { const WCHAR *name; size_t at; int type; float k; } visual_props[] = {
    { L"AnchorPoint", __builtin_offsetof(Obj, anchor), P_VEC2, 1 }, { L"CenterPoint", __builtin_offsetof(Obj, center), P_VEC3, 1 },
    { L"Offset", __builtin_offsetof(Obj, offset), P_VEC3, 1 }, { L"Opacity", __builtin_offsetof(Obj, opacity), P_SCALAR, 1 },
    { L"Orientation", __builtin_offsetof(Obj, orient), P_QUAT, 1 }, { L"RotationAngle", __builtin_offsetof(Obj, angle), P_SCALAR, 1 },
    { L"RotationAngleInDegrees", __builtin_offsetof(Obj, angle), P_SCALAR, 3.14159265358979f / 180 },
    { L"RotationAxis", __builtin_offsetof(Obj, axis), P_VEC3, 1 }, { L"Scale", __builtin_offsetof(Obj, scale), P_VEC3, 1 },
    { L"Size", __builtin_offsetof(Obj, size), P_VEC2, 1 }, { L"TransformMatrix", __builtin_offsetof(Obj, tm), P_M4X4, 1 },
    { L"RelativeOffsetAdjustment", __builtin_offsetof(Obj, rel_offset), P_VEC3, 1 },
    { L"RelativeSizeAdjustment", __builtin_offsetof(Obj, rel_size), P_VEC2, 1 },
};
static const WCHAR *const inset_props[] = { L"LeftInset", L"TopInset", L"RightInset", L"BottomInset" };
static const WCHAR *const rect_props[] = { L"Left", L"Top", L"Right", L"Bottom" };
/* @o's property @name (@n characters) (under the lock) */
static BOOL bind(Obj *o, const WCHAR *name, UINT32 n, Bind *b)
{
    __builtin_memset(b, 0, sizeof *b);
    b->k = 1;
    UINT32 base = 0;
    while (base < n && name[base] != '.') base++;
    int comp = -1;
    if (base < n) {
        if (n != base + 2) return FALSE;
        WCHAR c = name[base + 1];
        comp = c == 'X' ? 0 : c == 'Y' ? 1 : c == 'Z' ? 2 : c == 'W' ? 3 : -1;
        if (comp < 0) return FALSE;
    }
    if (o->kind == K_CONTAINER || o->kind == K_SPRITE) {
        for (size_t i = 0; i < sizeof visual_props / sizeof visual_props[0]; i++)
            if (wsame(visual_props[i].name, name, base)) {
                b->f = (float *)((char *)o + visual_props[i].at);
                b->type = visual_props[i].type;
                b->n = p_count[b->type];
                b->k = visual_props[i].k;
            }
        if (!b->f && wsame(L"IsVisible", name, base)) { b->b = &o->visible; b->type = P_BOOL; b->n = 1; }
    } else if (o->kind == K_COLOR_BRUSH && wsame(L"Color", name, base)) {
        b->color = &o->color; b->type = P_COLOR; b->n = 4;
    } else if (o->kind == K_INSET_CLIP || o->kind == K_RECT_CLIP) {
        for (int i = 0; i < 4; i++)
            if (wsame(o->kind == K_INSET_CLIP ? inset_props[i] : rect_props[i], name, base)) {
                b->f = &o->inset[i]; b->type = P_SCALAR; b->n = 1;
            }
    }
    if (!b->f && !b->b && !b->color) {
        Obj *ps = o->kind == K_PROPSET ? o : o->props;
        Prop *p = ps ? prop_find(ps, name, base) : 0;
        if (!p) return FALSE;
        b->f = p->v; b->type = p->type; b->n = p_count[p->type];
    }
    if (comp >= 0) {
        if (!b->f || comp >= b->n || b->n > 4) return FALSE;
        b->f += comp; b->n = 1; b->type = P_SCALAR;
    }
    return TRUE;
}
static void bind_read(const Bind *b, float *v)
{
    if (b->color) val_in(P_COLOR, b->color, v);
    else if (b->b) v[0] = *b->b ? 1.0f : 0.0f;
    else for (int i = 0; i < b->n; i++) v[i] = b->f[i] / b->k;
}
static void bind_write(const Bind *b, const float *v)
{
    if (b->color) val_out(P_COLOR, v, b->color);
    else if (b->b) *b->b = v[0] >= 0.5f;
    else for (int i = 0; i < b->n; i++) b->f[i] = v[i] * b->k;
}
static BOOL compatible(int a, int b) { return a == b || ((a == P_VEC4 || a == P_QUAT) && (b == P_VEC4 || b == P_QUAT)); }

typedef struct Active Active;
struct Active {
    Active *next;
    Obj *obj;                           /* the animated object (counted) */
    WCHAR *prop;
    Obj *anim;                          /* an expression animation (counted) */
    Obj *batch;                         /* the scoped batch it counts in (counted), or NULL */
    int type;                           /* the value's P_*; P_NONE: an expression */
    KF *kfs;
    int nkfs;
    float start[16];                    /* the property's value when it started */
    INT64 begin, delay, duration;
    int iters, forever, direction, stop_behavior, delay_behavior;
};
static Active *g_active;
static Obj *g_due;                      /* batches whose Completed is to be raised (counted, through due_next) */
static Obj **due_next(Obj *b) { return &b->next_due; }
static INT64 now_100ns(void) { return (INT64)GetTickCount64() * 10000; }

static void kf_value(const Active *a, const KF *k, float *v)
{
    __builtin_memcpy(v, k->kind == KF_VALUE ? k->v : a->start, sizeof k->v);
}
/* the animation's value at progress @t (0-1) */
static void anim_value(const Active *a, float t, float *v)
{
    const KF *right = 0, *left = 0;
    for (int i = 0; i < a->nkfs && !right; i++)
        if (a->kfs[i].t >= t) { right = &a->kfs[i]; left = i ? &a->kfs[i - 1] : 0; }
    if (!right) { kf_value(a, &a->kfs[a->nkfs - 1], v); return; }
    float l[16], r[16], lt = left ? left->t : 0;
    if (left) kf_value(a, left, l); else __builtin_memcpy(l, a->start, sizeof l);
    kf_value(a, right, r);
    float span = right->t - lt, u = span > 0 ? (t - lt) / span : 1;
    if (a->type == P_BOOL) { v[0] = u >= 1 ? r[0] : l[0]; return; }
    u = ease_at(&right->ease, u);
    int n = p_count[a->type];
    for (int i = 0; i < n; i++) v[i] = l[i] + (r[i] - l[i]) * u;
    if (a->type == P_QUAT) {
        float len = c_sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
        if (len > 0) for (int i = 0; i < 4; i++) v[i] /= len;
    }
}
/* where @a is at @now: -1 still in its delay, else 0 (*@t its progress) or
 * 1 (finished: *@t its progress at the end) */
static int anim_progress(const Active *a, INT64 now, float *t)
{
    INT64 el = now - a->begin - a->delay;
    if (el < 0) { *t = a->direction == 1 || a->direction == 3 ? 1.0f : 0.0f; return -1; }
    INT64 d = a->duration > 0 ? a->duration : 1, it = el / d;
    float frac = (float)(el - it * d) / (float)d;
    int done = 0;
    if (!a->forever && it >= a->iters) { it = a->iters - 1; frac = 1; done = 1; }
    /* AnimationDirection: Normal 0, Reverse 1, Alternate 2, AlternateReverse 3 */
    BOOL back = a->direction == 1 || (a->direction == 2 && (it & 1)) || (a->direction == 3 && !(it & 1));
    *t = back ? 1 - frac : frac;
    return done;
}
static void batch_due(Obj *b);
static void batch_done_one(Obj *b)      /* (under the lock) */
{
    if (--b->pending == 0 && b->b_ended) batch_due(b);
}
static void active_free(Active *a)      /* (under the lock: the releases may free) */
{
    if (a->batch) { batch_done_one(a->batch); obj_release(a->batch); }
    if (a->anim) obj_release(a->anim);
    obj_release(a->obj);
    HeapFree(GetProcessHeap(), 0, a->prop);
    HeapFree(GetProcessHeap(), 0, a->kfs);
    HeapFree(GetProcessHeap(), 0, a);
}
/* the animation on @o's property @name, if any, taken off; @behave: its
 * stop behaviour applied (LeaveCurrentValue 0, SetToInitialValue 1,
 * SetToFinalValue 2) (under the lock) */
static BOOL stop_named(Obj *o, const WCHAR *name, UINT32 n, BOOL behave)
{
    for (Active **pp = &g_active; *pp; pp = &(*pp)->next) {
        Active *a = *pp;
        if (a->obj != o || !wsame(a->prop, name, n)) continue;
        *pp = a->next;
        Bind b;
        if (behave && a->type && a->stop_behavior && bind(o, a->prop, n, &b)) {
            float v[16], t;
            if (a->stop_behavior == 1) __builtin_memcpy(v, a->start, sizeof v);
            else {
                Active end = *a;
                end.begin = 0; end.delay = 0; end.forever = 0;
                anim_progress(&end, (INT64)end.iters * (end.duration > 0 ? end.duration : 1), &t);
                anim_value(a, t, v);
            }
            bind_write(&b, v);
        }
        active_free(a);
        return TRUE;
    }
    return FALSE;
}
/* (the compositor's thread, under the lock) moves the key frame animations
 * on; TRUE while any runs */
static BOOL anim_tick(void)
{
    INT64 now = now_100ns();
    BOOL more = FALSE;
    for (Active **pp = &g_active; *pp;) {
        Active *a = *pp;
        if (!a->type) { pp = &a->next; continue; }
        Bind b;
        UINT32 n = 0;
        while (a->prop[n]) n++;
        if (!bind(a->obj, a->prop, n, &b)) { *pp = a->next; active_free(a); continue; }     /* (the property went) */
        float t, v[16];
        int st = anim_progress(a, now, &t);
        if (st >= 0 || a->delay_behavior == 1) {      /* (SetInitialValueBeforeDelay 1) */
            anim_value(a, t, v);
            bind_write(&b, v);
        }
        if (st > 0) { *pp = a->next; active_free(a); continue; }
        more = TRUE;
        pp = &a->next;
    }
    return more;
}
static HRESULT anim_start(Obj *o, HSTRING name, void *anim)
{
    UINT32 n;
    const WCHAR *c = hbuf(name, &n);
    if (!n) return E_INVALIDARG;
    Obj *a = obj_of(anim, K_EXPRESSION, K_KEYFRAME);
    if (!a) return E_INVALIDARG;
    Active *x = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *x);
    WCHAR *prop = wdup(c, n);
    HRESULT hr = x && prop ? S_OK : E_OUTOFMEMORY;
    lock();
    Bind b;
    if (SUCCEEDED(hr) && !bind(o, c, n, &b)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr) && a->kind == K_KEYFRAME && (!a->nkfs || !compatible(a->atype, b.type))) hr = E_INVALIDARG;
    if (SUCCEEDED(hr) && a->kind == K_KEYFRAME && !(x->kfs = HeapAlloc(GetProcessHeap(), 0, a->nkfs * sizeof *x->kfs))) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
        stop_named(o, c, n, FALSE);     /* (one already on the property ends) */
        x->obj = o; obj_addref(o);
        x->prop = prop; prop = 0;
        if (a->kind == K_KEYFRAME) {
            x->type = a->atype;
            __builtin_memcpy(x->kfs, a->kfs, a->nkfs * sizeof *x->kfs);
            x->nkfs = a->nkfs;
            bind_read(&b, x->start);
            x->begin = now_100ns();
            x->delay = a->delay; x->duration = a->duration;
            x->iters = a->iter_count > 0 ? a->iter_count : 1;
            x->forever = a->iter_behavior == 1;
            x->direction = a->direction; x->stop_behavior = a->stop_behavior; x->delay_behavior = a->delay_behavior;
            Obj *comp = o->kind == K_COMPOSITOR ? o : o->comp, *bt = comp ? comp->open_batch : 0;
            if (bt && bt->b_active) { x->batch = bt; obj_addref(bt); bt->pending++; }
        } else {
            x->anim = a; obj_addref(a);
        }
        x->next = g_active;
        g_active = x;
        x = 0;
    }
    unlock();
    obj_release(a);
    HeapFree(GetProcessHeap(), 0, prop);
    if (x) { HeapFree(GetProcessHeap(), 0, x->kfs); HeapFree(GetProcessHeap(), 0, x); }
    if (SUCCEEDED(hr)) { ensure_thread(); schedule(); }
    return hr;
}
static HRESULT anim_stop(Obj *o, HSTRING name)
{
    UINT32 n;
    const WCHAR *c = hbuf(name, &n);
    lock();
    stop_named(o, c, n, TRUE);
    unlock();
    schedule();
    return S_OK;
}
/* StartAnimationGroup / StopAnimationGroup: each animation of the group (or
 * the one animation) on the property its Target names */
static HRESULT group_run(Obj *o, void *group, BOOL start)
{
    Obj *g = obj_of(group, -1, -1);
    if (!g || (g->kind != K_GROUP && g->kind != K_EXPRESSION && g->kind != K_KEYFRAME)) { if (g) obj_release(g); return E_INVALIDARG; }
    Obj *one[1] = { g }, **list = one;
    int n = 1;
    lock();
    if (g->kind == K_GROUP) {
        n = g->nkids;
        list = n ? HeapAlloc(GetProcessHeap(), 0, n * sizeof *list) : 0;
        if (!list) n = 0;
        for (int i = 0; i < n; i++) { list[i] = g->kids[i]; obj_addref(list[i]); }
    } else obj_addref(g);
    unlock();
    HRESULT hr = S_OK;
    for (int i = 0; i < n; i++) {
        HSTRING target = 0;
        lock();
        if (list[i]->target_name) make_hstring(list[i]->target_name, &target);
        unlock();
        if (!target) hr = E_INVALIDARG;
        else if (start) { HRESULT h = anim_start(o, target, IF(list[i], 2)); if (FAILED(h)) hr = h; }
        else anim_stop(o, target);
        if (target) WindowsDeleteString(target);
        obj_release(list[i]);
    }
    if (list != one) HeapFree(GetProcessHeap(), 0, list);
    obj_release(g);
    return hr;
}

/* ---- batches --------------------------------------------------------------------
 * A scoped batch is open from CreateScopedBatch until Suspend or End; the
 * key frame animations started while it is open count in it, and once it
 * has ended and they have all finished, its Completed handlers run: on the
 * DispatcherQueue of the thread that made it, when that thread has one,
 * else on the compositor's thread.  A commit batch (GetCommitBatch) has
 * nothing to wait for: its Completed handlers run as they are added. */
static const GUID IID_IDispatcherQueueStatics_M = { 0xCD3382EA, 0xA455, 0x5124, { 0xB6, 0x3A, 0xCA, 0x40, 0xD3, 0x4C, 0xA2, 0x3C } };
static const GUID IID_IDispatcherQueueStatics_W = { 0xA96D83D7, 0x9371, 0x4517, { 0x92, 0x45, 0xD0, 0x82, 0x4A, 0xC1, 0x2C, 0x74 } };
/* the calling thread's DispatcherQueue (Microsoft.UI.Dispatching's or
 * Windows.System's; NULL when it has none) */
static HRESULT thread_queue(BOOL ms, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    HSTRING s = 0;
    void *statics = 0;
    make_hstring(ms ? L"Microsoft.UI.Dispatching.DispatcherQueue" : L"Windows.System.DispatcherQueue", &s);
    HRESULT hr = RoGetActivationFactory(s, ms ? &IID_IDispatcherQueueStatics_M : &IID_IDispatcherQueueStatics_W, &statics);
    WindowsDeleteString(s);
    if (FAILED(hr) || !statics) return S_OK;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))(*(void ***)statics)[6])(statics, out);     /* GetForCurrentThread */
    ((IUnknown *)statics)->lpVtbl->Release((IUnknown *)statics);
    return hr;
}
/* runs @b's Completed handlers */
static void batch_invoke(Obj *b)
{
    lock();
    int n = b->nhandlers;
    IUnknown **hs = n ? HeapAlloc(GetProcessHeap(), 0, n * sizeof *hs) : 0;
    if (!hs) n = 0;
    for (int i = 0; i < n; i++) { hs[i] = b->handlers[i].h; hs[i]->lpVtbl->AddRef(hs[i]); }
    unlock();
    Obj *args = n ? obj_new(b->comp, K_BATCH_ARGS) : 0;
    if (args) args->vt[2] = empty2_vt;
    for (int i = 0; i < n; i++) {
        if (args) ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, void *))((void **)*(void **)hs[i])[3])(hs[i], IF(b, 2), IF(args, 2));
        hs[i]->lpVtbl->Release(hs[i]);
    }
    if (args) obj_release(args);
    HeapFree(GetProcessHeap(), 0, hs);
}
/* the work a batch's DispatcherQueue runs (a DispatcherQueueHandler) */
typedef struct { const void *const *vt; LONG refs; Obj *batch; } Work;
static HRESULT STDMETHODCALLTYPE w_qi(void *p, REFIID iid, void **out)
{
    (void)iid;
    if (!out) return E_POINTER;
    *out = p;
    InterlockedIncrement(&((Work *)p)->refs);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE w_addref(void *p) { return (ULONG)InterlockedIncrement(&((Work *)p)->refs); }
static ULONG STDMETHODCALLTYPE w_release(void *p)
{
    Work *w = p;
    LONG r = InterlockedDecrement(&w->refs);
    if (!r) { obj_release(w->batch); HeapFree(GetProcessHeap(), 0, w); }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE w_invoke(void *p) { batch_invoke(((Work *)p)->batch); return S_OK; }
static const void *const work_vt[] = { (void *)w_qi, (void *)w_addref, (void *)w_release, (void *)w_invoke };
/* (the compositor's thread) raises the due batches' Completed */
static void batches_fire(void)
{
    lock();
    Obj *b = g_due;
    g_due = 0;
    unlock();
    while (b) {
        Obj *next = *due_next(b);
        *due_next(b) = 0;
        boolean_t queued = 0;
        Work *w = b->queue ? HeapAlloc(GetProcessHeap(), 0, sizeof *w) : 0;
        if (w) {
            w->vt = work_vt; w->refs = 1; w->batch = b;
            obj_addref(b);
            ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, boolean_t *))((void **)*(void **)b->queue)[7])(b->queue, w, &queued);  /* TryEnqueue */
            w_release(w);
        }
        if (!queued) batch_invoke(b);
        obj_release(b);
        b = next;
    }
}
static void batch_due(Obj *b)           /* (under the lock) */
{
    if (b->b_fired) return;
    b->b_fired = TRUE;
    obj_addref(b);
    *due_next(b) = g_due;
    g_due = b;
}
#define BT(p) OBJ_AT(p, 2)
static HRESULT STDMETHODCALLTYPE bt_active(void *p, boolean_t *v) { if (!v) return E_POINTER; *v = BT(p)->b_active; return S_OK; }
static HRESULT STDMETHODCALLTYPE bt_ended(void *p, boolean_t *v) { if (!v) return E_POINTER; *v = BT(p)->b_ended; return S_OK; }
static HRESULT STDMETHODCALLTYPE bt_end(void *p)
{
    Obj *b = BT(p);
    lock();
    HRESULT hr = b->b_ended ? E_ILLEGAL_METHOD_CALL_ : S_OK;
    if (SUCCEEDED(hr)) {
        b->b_ended = TRUE;
        b->b_active = FALSE;
        if (b->comp->open_batch == b) b->comp->open_batch = 0;
        if (!b->pending) batch_due(b);
    }
    unlock();
    if (SUCCEEDED(hr)) { ensure_thread(); schedule(); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE bt_resume(void *p)
{
    Obj *b = BT(p);
    lock();
    HRESULT hr = b->b_ended ? E_ILLEGAL_METHOD_CALL_ : S_OK;
    if (SUCCEEDED(hr)) { b->b_active = TRUE; b->comp->open_batch = b; }
    unlock();
    return hr;
}
static HRESULT STDMETHODCALLTYPE bt_suspend(void *p)
{
    Obj *b = BT(p);
    lock();
    HRESULT hr = b->b_ended ? E_ILLEGAL_METHOD_CALL_ : S_OK;
    if (SUCCEEDED(hr)) { b->b_active = FALSE; if (b->comp->open_batch == b) b->comp->open_batch = 0; }
    unlock();
    return hr;
}
static LONG g_tokens;
static HRESULT STDMETHODCALLTYPE bt_add(void *p, IUnknown *h, EventRegistrationToken *t)
{
    Obj *b = BT(p);
    if (!h || !t) return E_INVALIDARG;
    HRESULT hr = S_OK;
    lock();
    if (!grow((void **)&b->handlers, b->nhandlers, sizeof *b->handlers)) hr = E_OUTOFMEMORY;
    else {
        h->lpVtbl->AddRef(h);
        t->value = InterlockedIncrement(&g_tokens);
        b->handlers[b->nhandlers].h = h;
        b->handlers[b->nhandlers++].token = *t;
        if (b->b_commit) batch_due(b);
    }
    unlock();
    if (b->b_commit) { ensure_thread(); schedule(); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE bt_remove(void *p, EventRegistrationToken t)
{
    Obj *b = BT(p);
    IUnknown *h = 0;
    lock();
    for (int i = 0; i < b->nhandlers; i++)
        if (b->handlers[i].token.value == t.value) { h = b->handlers[i].h; b->handlers[i] = b->handlers[--b->nhandlers]; break; }
    unlock();
    if (h) h->lpVtbl->Release(h);
    return S_OK;
}
static const void *const scoped_vt[] = { BASE(2), (void *)bt_active, (void *)bt_ended, (void *)bt_end, (void *)bt_resume,
                                         (void *)bt_suspend, (void *)bt_add, (void *)bt_remove };
static const void *const commit_vt[] = { BASE(2), (void *)bt_active, (void *)bt_ended, (void *)bt_add, (void *)bt_remove };
/* CompositionBatchTypes: None 0, Animation 1, Effect 2, InfiniteAnimation 4, AllAnimations 5 */
static HRESULT batch_new(Obj *comp, int types, BOOL commit, void **out)
{
    (void)types;
    if (!out) return E_POINTER;
    *out = 0;
    Obj *b = obj_new(comp, K_BATCH);
    if (!b) return E_OUTOFMEMORY;
    b->vt[2] = commit ? commit_vt : scoped_vt;
    b->b_commit = commit != 0;
    void *q = 0;
    thread_queue(TRUE, &q);
    if (!q) thread_queue(FALSE, &q);
    b->queue = q;
    lock();
    if (commit) b->b_ended = TRUE;
    else { b->b_active = TRUE; comp->open_batch = b; }
    unlock();
    *out = IF(b, 2);
    return S_OK;
}

/* ---- rectangle clips: IRectangleClip (slot 3) -------------------------------------
 * Left, Top, Right, Bottom (in the visual's space), the corners' radii (kept,
 * not drawn: the clip is square-cornered) */
#define RSIDE(name, i) \
    static HRESULT STDMETHODCALLTYPE rc_get_##name(void *p, float *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 3)->inset[i]; return S_OK; } \
    static HRESULT STDMETHODCALLTYPE rc_put_##name(void *p, float v) { lock(); OBJ_AT(p, 3)->inset[i] = v; unlock(); schedule(); return S_OK; }
#define RCORNER(name, i) \
    static HRESULT STDMETHODCALLTYPE rc_get_##name(void *p, Vec2 *v) { if (!v) return E_POINTER; *v = OBJ_AT(p, 3)->radius[i]; return S_OK; } \
    static HRESULT STDMETHODCALLTYPE rc_put_##name(void *p, Vec2 v) { OBJ_AT(p, 3)->radius[i] = v; return S_OK; }
RSIDE(left, 0) RSIDE(top, 1) RSIDE(right, 2) RSIDE(bottom, 3) RCORNER(tl, 0) RCORNER(tr, 1) RCORNER(br, 2) RCORNER(bl, 3)
static const void *const rect_clip_vt[] = {
    BASE(3), (void *)rc_get_bottom, (void *)rc_put_bottom, (void *)rc_get_bl, (void *)rc_put_bl, (void *)rc_get_br, (void *)rc_put_br,
    (void *)rc_get_left, (void *)rc_put_left, (void *)rc_get_right, (void *)rc_put_right, (void *)rc_get_top, (void *)rc_put_top,
    (void *)rc_get_tl, (void *)rc_put_tl, (void *)rc_get_tr, (void *)rc_put_tr,
};
static HRESULT rect_clip_new(Obj *comp, float l, float t, float r, float b, const Vec2 *radii, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *c = obj_new(comp, K_RECT_CLIP);
    if (!c) return E_OUTOFMEMORY;
    c->vt[2] = empty2_vt; c->vt[3] = rect_clip_vt; c->vt[4] = surface_brush2_vt;
    c->inset[0] = l; c->inset[1] = t; c->inset[2] = r; c->inset[3] = b;
    if (radii) for (int i = 0; i < 4; i++) c->radius[i] = radii[i];
    *out = IF(c, 3);
    return S_OK;
}

/* ---- the Compositor ------------------------------------------------------------------ */
#define COMP(p, i) OBJ_AT(p, i)
static HRESULT not_made(void **out) { if (out) *out = 0; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE c_none(void *p, void **out) { (void)p; return not_made(out); }
static HRESULT STDMETHODCALLTYPE c_none_easing(void *p, Vec2 a, Vec2 b, void **out) { (void)p; (void)a; (void)b; return not_made(out); }
static HRESULT STDMETHODCALLTYPE c_none_effect(void *p, void *e, void **out) { (void)p; (void)e; return not_made(out); }
static HRESULT STDMETHODCALLTYPE c_none_effect2(void *p, void *e, void *names, void **out) { (void)p; (void)e; (void)names; return not_made(out); }
static HRESULT STDMETHODCALLTYPE c_none_expr(void *p, HSTRING s, void **out) { (void)p; (void)s; return not_made(out); }
static HRESULT STDMETHODCALLTYPE c_none_batch(void *p, int type, void **out) { (void)p; (void)type; return not_made(out); }

static HRESULT new_visual(Obj *comp, int kind, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *v = obj_new(comp, kind);
    if (!v) return E_OUTOFMEMORY;
    v->vt[0] = co_vt; v->vt[1] = closable_vt; v->vt[2] = visual_vt; v->vt[3] = container_vt;
    if (kind == K_SPRITE) v->vt[4] = sprite_vt;
    v->vt[5] = visual2_vt; v->vt[6] = visual3_vt; v->vt[7] = visual4_vt;
    v->opacity = 1.0f;
    v->visible = 1;
    v->hit_test = 1;
    v->scale.X = v->scale.Y = v->scale.Z = 1.0f;
    v->axis.Z = 1.0f;
    v->orient.W = 1.0f;
    for (int i = 0; i < 4; i++) v->tm.m[i][i] = 1.0f;
    *out = IF(v, kind == K_SPRITE ? 4 : 3);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_container(void *p, void **out) { return new_visual(COMP(p, 0), K_CONTAINER, out); }
static HRESULT STDMETHODCALLTYPE c_sprite(void *p, void **out) { return new_visual(COMP(p, 0), K_SPRITE, out); }
static HRESULT STDMETHODCALLTYPE c_color_brush_with(void *p, Color c, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *b = obj_new(COMP(p, 0), K_COLOR_BRUSH);
    if (!b) return E_OUTOFMEMORY;
    b->vt[0] = co_vt; b->vt[1] = closable_vt; b->vt[2] = empty2_vt; b->vt[3] = color_vt;
    b->color = c;
    *out = IF(b, 3);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_color_brush(void *p, void **out) { Color black = { 255, 0, 0, 0 }; return c_color_brush_with(p, black, out); }
static HRESULT STDMETHODCALLTYPE c_surface_brush_with(void *p, void *surface, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *s = 0;
    if (surface && !(s = obj_of(surface, K_DRAWING_SURFACE, K_CHAIN_SURFACE))) return E_INVALIDARG;
    Obj *b = obj_new(COMP(p, 0), K_SURFACE_BRUSH);
    if (!b) { if (s) obj_release(s); return E_OUTOFMEMORY; }
    b->vt[0] = co_vt; b->vt[1] = closable_vt; b->vt[2] = empty2_vt; b->vt[3] = surface_brush_vt;
    b->vt[4] = surface_brush2_vt; b->vt[5] = surface_brush3_vt;
    b->surface = s;
    b->stretch = 1;                     /* Fill */
    b->interp = 1;                      /* Linear (sampled nearest here) */
    b->hratio = b->vratio = 0.5f;
    *out = IF(b, 3);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_surface_brush(void *p, void **out) { return c_surface_brush_with(p, 0, out); }
static HRESULT STDMETHODCALLTYPE c_inset_with(void *p, float l, float t, float r, float b, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *c = obj_new(COMP(p, 0), K_INSET_CLIP);
    if (!c) return E_OUTOFMEMORY;
    c->vt[0] = co_vt; c->vt[1] = closable_vt; c->vt[2] = empty2_vt; c->vt[3] = inset_vt; c->vt[4] = surface_brush2_vt;
    c->inset[0] = l; c->inset[1] = t; c->inset[2] = r; c->inset[3] = b;
    *out = IF(c, 3);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c_inset(void *p, void **out) { return c_inset_with(p, 0, 0, 0, 0, out); }
/* the thread's CoreWindow (twinapi.appcore.dll's) */
static HRESULT STDMETHODCALLTYPE c_target_for_view(void *p, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    static const WCHAR cls[] = L"Windows.UI.Core.CoreWindow";
    HSTRING s = 0;
    void *statics = 0, *win = 0, *interop = 0;
    HWND h = 0;
    WindowsCreateString(cls, sizeof cls / sizeof cls[0] - 1, &s);
    HRESULT hr = RoGetActivationFactory(s, &IID_ICoreWindowStatic_, &statics);
    WindowsDeleteString(s);
    if (SUCCEEDED(hr)) hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))(*(void ***)statics)[6])(statics, &win);   /* GetForCurrentThread */
    if (SUCCEEDED(hr) && !win) hr = E_ILLEGAL_METHOD_CALL_;                      /* (no CoreWindow on this thread) */
    if (SUCCEEDED(hr)) hr = ((IUnknown *)win)->lpVtbl->QueryInterface((IUnknown *)win, &IID_ICoreWindowInterop_, &interop);
    if (SUCCEEDED(hr)) hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, HWND *))(*(void ***)interop)[3])(interop, &h);
    if (interop) ((IUnknown *)interop)->lpVtbl->Release((IUnknown *)interop);
    if (win) ((IUnknown *)win)->lpVtbl->Release((IUnknown *)win);
    if (statics) ((IUnknown *)statics)->lpVtbl->Release((IUnknown *)statics);
    if (FAILED(hr)) return hr;
    Obj *t;
    hr = target_new(COMP(p, 0), h, FALSE, 0, &t);
    if (SUCCEEDED(hr)) *out = IF(t, 2);
    return hr;
}
/* ICompositor's animations, easing functions, property sets and batches */
static HRESULT STDMETHODCALLTYPE c_kf_color(void *p, void **out) { return anim_new(COMP(p, 0), K_KEYFRAME, P_COLOR, 0, out); }
static HRESULT STDMETHODCALLTYPE c_kf_quat(void *p, void **out) { return anim_new(COMP(p, 0), K_KEYFRAME, P_QUAT, 0, out); }
static HRESULT STDMETHODCALLTYPE c_kf_scalar(void *p, void **out) { return anim_new(COMP(p, 0), K_KEYFRAME, P_SCALAR, 0, out); }
static HRESULT STDMETHODCALLTYPE c_kf_vec2(void *p, void **out) { return anim_new(COMP(p, 0), K_KEYFRAME, P_VEC2, 0, out); }
static HRESULT STDMETHODCALLTYPE c_kf_vec3(void *p, void **out) { return anim_new(COMP(p, 0), K_KEYFRAME, P_VEC3, 0, out); }
static HRESULT STDMETHODCALLTYPE c_kf_vec4(void *p, void **out) { return anim_new(COMP(p, 0), K_KEYFRAME, P_VEC4, 0, out); }
static HRESULT STDMETHODCALLTYPE c_expression(void *p, void **out) { return anim_new(COMP(p, 0), K_EXPRESSION, P_NONE, 0, out); }
static HRESULT STDMETHODCALLTYPE c_expression_with(void *p, HSTRING e, void **out) { return anim_new(COMP(p, 0), K_EXPRESSION, P_NONE, e, out); }
static HRESULT STDMETHODCALLTYPE c_cubic(void *p, Vec2 a, Vec2 b, void **out) { return easing_cubic(COMP(p, 0), a, b, out); }
static HRESULT STDMETHODCALLTYPE c_linear(void *p, void **out) { return easing_kind(COMP(p, 0), E_LINEAR, 0, 0, out); }
static HRESULT STDMETHODCALLTYPE c_propset(void *p, void **out)
{
    if (!out) return E_POINTER;
    Obj *o = propset_new(COMP(p, 0));
    *out = o ? IF(o, 2) : 0;
    return o ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE c_scoped(void *p, int types, void **out) { return batch_new(COMP(p, 0), types, FALSE, out); }
static HRESULT STDMETHODCALLTYPE c_commit(void *p, int types, void **out) { return batch_new(COMP(p, 0), types, TRUE, out); }
static const void *const compositor_vt[] = {
    BASE(0),
    (void *)c_kf_color, (void *)c_color_brush, (void *)c_color_brush_with, (void *)c_container, (void *)c_cubic,
    (void *)c_none_effect, (void *)c_none_effect2, (void *)c_expression, (void *)c_expression_with, (void *)c_inset, (void *)c_inset_with,
    (void *)c_linear, (void *)c_propset, (void *)c_kf_quat, (void *)c_kf_scalar, (void *)c_scoped, (void *)c_sprite, (void *)c_surface_brush,
    (void *)c_surface_brush_with, (void *)c_target_for_view, (void *)c_kf_vec2, (void *)c_kf_vec3, (void *)c_kf_vec4, (void *)c_commit,
};
_Static_assert(sizeof compositor_vt / sizeof compositor_vt[0] == 6 + 24, "ICompositor has 24 methods");
/* Microsoft.UI's ICompositor (slot S_C_M): the same less CreateTargetForCurrentView */
#define CM(p) ((char *)(p) - (S_C_M) * sizeof(void *))
static HRESULT STDMETHODCALLTYPE cm_kf_color(void *p, void **out) { return c_kf_color(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_color_brush(void *p, void **out) { return c_color_brush(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_color_brush_with(void *p, Color c, void **out) { return c_color_brush_with(CM(p), c, out); }
static HRESULT STDMETHODCALLTYPE cm_container(void *p, void **out) { return c_container(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_cubic(void *p, Vec2 a, Vec2 b, void **out) { return c_cubic(CM(p), a, b, out); }
static HRESULT STDMETHODCALLTYPE cm_expression(void *p, void **out) { return c_expression(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_expression_with(void *p, HSTRING e, void **out) { return c_expression_with(CM(p), e, out); }
static HRESULT STDMETHODCALLTYPE cm_inset(void *p, void **out) { return c_inset(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_inset_with(void *p, float l, float t, float r, float b, void **out) { return c_inset_with(CM(p), l, t, r, b, out); }
static HRESULT STDMETHODCALLTYPE cm_linear(void *p, void **out) { return c_linear(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_propset(void *p, void **out) { return c_propset(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_kf_quat(void *p, void **out) { return c_kf_quat(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_kf_scalar(void *p, void **out) { return c_kf_scalar(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_scoped(void *p, int types, void **out) { return c_scoped(CM(p), types, out); }
static HRESULT STDMETHODCALLTYPE cm_sprite(void *p, void **out) { return c_sprite(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_surface_brush(void *p, void **out) { return c_surface_brush(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_surface_brush_with(void *p, void *s, void **out) { return c_surface_brush_with(CM(p), s, out); }
static HRESULT STDMETHODCALLTYPE cm_kf_vec2(void *p, void **out) { return c_kf_vec2(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_kf_vec3(void *p, void **out) { return c_kf_vec3(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_kf_vec4(void *p, void **out) { return c_kf_vec4(CM(p), out); }
static HRESULT STDMETHODCALLTYPE cm_commit(void *p, int types, void **out) { return c_commit(CM(p), types, out); }
static const void *const compositor_m_vt[] = {
    BASE(4),
    (void *)cm_kf_color, (void *)cm_color_brush, (void *)cm_color_brush_with, (void *)cm_container, (void *)cm_cubic,
    (void *)c_none_effect, (void *)c_none_effect2, (void *)cm_expression, (void *)cm_expression_with, (void *)cm_inset, (void *)cm_inset_with,
    (void *)cm_linear, (void *)cm_propset, (void *)cm_kf_quat, (void *)cm_kf_scalar, (void *)cm_scoped, (void *)cm_sprite, (void *)cm_surface_brush,
    (void *)cm_surface_brush_with, (void *)cm_kf_vec2, (void *)cm_kf_vec3, (void *)cm_kf_vec4, (void *)cm_commit,
};
_Static_assert(sizeof compositor_m_vt / sizeof compositor_m_vt[0] == 6 + 23 && S_C_M == 4, "Microsoft.UI's ICompositor has 23 methods");
static HRESULT STDMETHODCALLTYPE c_close(void *p) { (void)p; return S_OK; }
static const void *const compositor_closable_vt[] = { BASE(1), (void *)c_close };

/* ICompositor2 (slot S_C2): CreateAnimationGroup, CreateStepEasingFunction; lights, shadows,
 * mask brushes; lights, shadows, layer visuals, nine-grid and backdrop brushes and implicit
 * animations are not here */
static HRESULT STDMETHODCALLTYPE c2_group(void *p, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *g = obj_new(COMP(p, S_C2), K_GROUP);
    if (!g) return E_OUTOFMEMORY;
    g->vt[2] = group_vt; g->vt[3] = empty3_vt;
    *out = IF(g, 2);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE c2_step(void *p, void **out) { return easing_step(COMP(p, S_C2), 1, out); }
static HRESULT STDMETHODCALLTYPE c2_step_n(void *p, int n, void **out) { return easing_step(COMP(p, S_C2), n, out); }
static HRESULT STDMETHODCALLTYPE c2_mask_brush(void *p, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *b = obj_new(COMP(p, S_C2), K_MASK_BRUSH);
    if (!b) return E_OUTOFMEMORY;
    b->vt[0] = co_vt; b->vt[1] = closable_vt; b->vt[2] = empty2_vt; b->vt[3] = mask_brush_vt;
    *out = IF(b, 3);
    return S_OK;
}
static const void *const compositor2_vt[] = { BASE(5), (void *)c_none, (void *)c2_group, (void *)c_none, (void *)c_none, (void *)c_none,
                                              (void *)c_none, (void *)c_none, (void *)c2_mask_brush, (void *)c_none, (void *)c_none, (void *)c_none,
                                              (void *)c2_step, (void *)c2_step_n };
/* ICompositor3 (slot S_C3): CreateHostBackdropBrush (not here) */
static const void *const compositor3_vt[] = { BASE(6), (void *)c_none };
/* ICompositor4 (slot S_C4): gradient stops and brushes and spring animations (not here) */
static HRESULT STDMETHODCALLTYPE c_none_stop(void *p, float at, Color c, void **out) { (void)p; (void)at; (void)c; return not_made(out); }
static const void *const compositor4_vt[] = { BASE(7), (void *)c_none, (void *)c_none_stop, (void *)c_none, (void *)c_none, (void *)c_none,
                                              (void *)c_none };
/* ICompositor5 (slot S_C5): Comment, GlobalPlaybackRate (kept); shapes, bounce and
 * path animations and RequestCommitAsync are not here */
static HRESULT STDMETHODCALLTYPE c5_get_comment(void *p, HSTRING *out) { return comment_get(COMP(p, S_C5), out); }
static HRESULT STDMETHODCALLTYPE c5_put_comment(void *p, HSTRING s) { return comment_put(COMP(p, S_C5), s); }
static HRESULT STDMETHODCALLTYPE c5_get_rate(void *p, float *v) { if (!v) return E_POINTER; *v = COMP(p, S_C5)->playback_rate; return S_OK; }
static HRESULT STDMETHODCALLTYPE c5_put_rate(void *p, float v) { COMP(p, S_C5)->playback_rate = v; return S_OK; }
static const void *const compositor5_vt[] = {
    BASE(8), (void *)c5_get_comment, (void *)c5_put_comment, (void *)c5_get_rate, (void *)c5_put_rate,
    (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none_effect,
    (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none, (void *)c_none_effect, (void *)c_none, (void *)c_none,
};
/* ICompositor6 (slot S_C6): CreateBooleanKeyFrameAnimation; geometric clips and redirect visuals are not here */
static HRESULT STDMETHODCALLTYPE c6_kf_bool(void *p, void **out) { return anim_new(COMP(p, S_C6), K_KEYFRAME, P_BOOL, 0, out); }
static const void *const compositor6_vt[] = { BASE(9), (void *)c_none, (void *)c_none_effect, (void *)c_none, (void *)c_none_effect,
                                              (void *)c6_kf_bool };
/* ICompositor7 (slots S_C7_W, S_C7_M): DispatcherQueue (the calling thread's),
 * CreateRectangleClip; CreateAnimationPropertyInfo is not here */
static HRESULT STDMETHODCALLTYPE c7_w_queue(void *p, void **out) { (void)p; return thread_queue(FALSE, out); }
static HRESULT STDMETHODCALLTYPE c7_m_queue(void *p, void **out) { (void)p; return thread_queue(TRUE, out); }
#define C7(name, slot) \
    static HRESULT STDMETHODCALLTYPE c7_rect_##name(void *p, void **out) { return rect_clip_new(COMP(p, slot), 0, 0, 0, 0, 0, out); } \
    static HRESULT STDMETHODCALLTYPE c7_rect4_##name(void *p, float l, float t, float r, float b, void **out) \
    { return rect_clip_new(COMP(p, slot), l, t, r, b, 0, out); } \
    static HRESULT STDMETHODCALLTYPE c7_rect8_##name(void *p, float l, float t, float r, float b, Vec2 tl, Vec2 tr, Vec2 br, Vec2 bl, void **out) \
    { Vec2 k[4] = { tl, tr, br, bl }; return rect_clip_new(COMP(p, slot), l, t, r, b, k, out); }
C7(w, S_C7_W) C7(m, S_C7_M)
static const void *const compositor7_w_vt[] = { BASE(10), (void *)c7_w_queue, (void *)c_none, (void *)c7_rect_w, (void *)c7_rect4_w,
                                                (void *)c7_rect8_w };
static const void *const compositor7_m_vt[] = { BASE(11), (void *)c7_m_queue, (void *)c_none, (void *)c7_rect_m, (void *)c7_rect4_m,
                                                (void *)c7_rect8_m };
/* ICompositor8 (slot S_C8): CreateAnimationController (not here) */
static const void *const compositor8_vt[] = { BASE(12), (void *)c_none };
_Static_assert(S_C2 == 5 && S_C3 == 6 && S_C4 == 7 && S_C5 == 8 && S_C6 == 9 && S_C7_W == 10 && S_C7_M == 11 && S_C8 == 12,
               "the BASE(n) above");

/* ICompositorInterop (slot 2) */
static HRESULT STDMETHODCALLTYPE ci_for_handle(void *p, HANDLE h, void **out) { (void)p; (void)h; return not_made(out); }
static HRESULT STDMETHODCALLTYPE ci_for_chain(void *p, IUnknown *chain, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    INovaSwapChain *nc = 0;
    if (!chain || FAILED(chain->lpVtbl->QueryInterface(chain, &NIID_INovaSwapChain, (void **)&nc)) || !nc) return E_INVALIDARG;
    Obj *s = obj_new(COMP(p, 2), K_CHAIN_SURFACE);
    if (!s) { nc->lpVtbl->Release(nc); return E_OUTOFMEMORY; }
    s->vt[0] = co_vt; s->vt[1] = closable_vt; s->vt[2] = empty2_vt;
    s->chain = nc;
    nc->lpVtbl->SetSink(nc, chain_sink, s);
    *out = IF(s, 2);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ci_graphics(void *p, IUnknown *dev, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    IUnknown *d3d, *ctx, *d2d;
    HRESULT hr = d3d_of(dev, &d3d, &ctx, &d2d);
    if (FAILED(hr)) return hr;
    Obj *g = obj_new(COMP(p, 2), K_GRAPHICS);
    if (!g) {
        ctx->lpVtbl->Release(ctx); d3d->lpVtbl->Release(d3d);
        if (d2d) d2d->lpVtbl->Release(d2d);
        return E_OUTOFMEMORY;
    }
    g->vt[0] = co_vt; g->vt[1] = closable_vt; g->vt[2] = graphics_vt; g->vt[3] = graphics_interop_vt;
    g->d3d = d3d; g->ctx = ctx; g->d2d = d2d;
    *out = IF(g, 2);
    return S_OK;
}
static const void *const compositor_interop_vt[] = { UNK(2), (void *)ci_for_handle, (void *)ci_for_chain, (void *)ci_graphics };

/* ICompositorDesktopInterop (slot 3) */
static HRESULT STDMETHODCALLTYPE cd_target(void *p, HWND h, BOOL topmost, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *t;
    HRESULT hr = target_new(COMP(p, 3), h, TRUE, topmost ? 1 : 0, &t);
    if (SUCCEEDED(hr)) *out = IF(t, 3);
    return hr;
}
static HRESULT STDMETHODCALLTYPE cd_ensure(void *p, DWORD tid) { (void)p; (void)tid; return S_OK; }
static const void *const compositor_desktop_vt[] = { UNK(3), (void *)cd_target, (void *)cd_ensure };

/* ---- the activation factory ------------------------------------------------------------ */
static HRESULT STDMETHODCALLTYPE f_qi(void *p, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_IInspectable_) || IsEqualGUID(iid, &IID_IAgileObject_) ||
        IsEqualGUID(iid, &IID_IActivationFactory_)) { *out = p; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE f_ref(void *p) { (void)p; return 2; }
static HRESULT STDMETHODCALLTYPE f_name(void *p, HSTRING *n) { (void)p; return make_hstring(L"Windows.UI.Composition.Compositor", n); }
static HRESULT STDMETHODCALLTYPE f_activate(void *p, void **out)
{
    (void)p;
    if (!out) return E_POINTER;
    *out = 0;
    Obj *c = obj_new(0, K_COMPOSITOR);
    if (!c) return E_OUTOFMEMORY;
    c->vt[0] = compositor_vt; c->vt[1] = compositor_closable_vt; c->vt[2] = compositor_interop_vt; c->vt[3] = compositor_desktop_vt;
    c->vt[S_C_M] = compositor_m_vt; c->vt[S_C2] = compositor2_vt; c->vt[S_C3] = compositor3_vt; c->vt[S_C4] = compositor4_vt;
    c->vt[S_C5] = compositor5_vt; c->vt[S_C6] = compositor6_vt; c->vt[S_C7_W] = compositor7_w_vt; c->vt[S_C7_M] = compositor7_m_vt;
    c->vt[S_C8] = compositor8_vt;
    c->playback_rate = 1.0f;
    *out = IF(c, 0);
    return S_OK;
}
static const void *const factory_vt[] = { (void *)f_qi, (void *)f_ref, (void *)f_ref, (void *)no_iids, (void *)f_name, (void *)base_trust,
                                          (void *)f_activate };
static const void *const *const g_factory = factory_vt;

/* CompositionEasingFunction's statics (ICompositionEasingFunctionStatics, in
 * either namespace): the easing functions of a compositor */
static HRESULT STDMETHODCALLTYPE es_qi(void *p, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE es_ref(void *p) { (void)p; return 2; }
static HRESULT STDMETHODCALLTYPE es_name(void *p, HSTRING *n) { (void)p; return make_hstring(L"Windows.UI.Composition.CompositionEasingFunction", n); }
static HRESULT STDMETHODCALLTYPE es_activate(void *p, void **out) { (void)p; return not_made(out); }
static Obj *comp_of(void *c) { Obj *o = obj_of(c, K_COMPOSITOR, -1); if (o) obj_release(o); return o; }    /* (the caller holds it) */
#define ES_COMP(c) Obj *o = comp_of(c); if (!o) { if (out) *out = 0; return E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE es_cubic(void *p, void *c, Vec2 a, Vec2 b, void **out) { (void)p; ES_COMP(c) return easing_cubic(o, a, b, out); }
static HRESULT STDMETHODCALLTYPE es_linear(void *p, void *c, void **out) { (void)p; ES_COMP(c) return easing_kind(o, E_LINEAR, 0, 0, out); }
static HRESULT STDMETHODCALLTYPE es_step(void *p, void *c, void **out) { (void)p; ES_COMP(c) return easing_step(o, 1, out); }
static HRESULT STDMETHODCALLTYPE es_step_n(void *p, void *c, int n, void **out) { (void)p; ES_COMP(c) return easing_step(o, n, out); }
static HRESULT STDMETHODCALLTYPE es_back(void *p, void *c, int mode, float amp, void **out) { (void)p; (void)amp; ES_COMP(c) return easing_kind(o, E_OTHER, mode, 0, out); }
static HRESULT STDMETHODCALLTYPE es_bounce(void *p, void *c, int mode, int n, float k, void **out) { (void)p; (void)n; (void)k; ES_COMP(c) return easing_kind(o, E_OTHER, mode, 0, out); }
static HRESULT STDMETHODCALLTYPE es_circle(void *p, void *c, int mode, void **out) { (void)p; ES_COMP(c) return easing_kind(o, E_CIRCLE, mode, 0, out); }
static HRESULT STDMETHODCALLTYPE es_elastic(void *p, void *c, int mode, int n, float k, void **out) { (void)p; (void)n; (void)k; ES_COMP(c) return easing_kind(o, E_OTHER, mode, 0, out); }
static HRESULT STDMETHODCALLTYPE es_exp(void *p, void *c, int mode, float e, void **out) { (void)p; (void)e; ES_COMP(c) return easing_kind(o, E_OTHER, mode, 0, out); }
static HRESULT STDMETHODCALLTYPE es_power(void *p, void *c, int mode, float k, void **out) { (void)p; ES_COMP(c) return easing_kind(o, E_POWER, mode, k, out); }
static HRESULT STDMETHODCALLTYPE es_sine(void *p, void *c, int mode, void **out) { (void)p; ES_COMP(c) return easing_kind(o, E_SINE, mode, 0, out); }
static const void *const easing_factory_vt[] = { (void *)es_qi, (void *)es_ref, (void *)es_ref, (void *)no_iids, (void *)es_name,
                                                 (void *)base_trust, (void *)es_activate };
static const void *const easing_statics_vt[] = { (void *)es_qi, (void *)es_ref, (void *)es_ref, (void *)no_iids, (void *)es_name,
                                                 (void *)base_trust, (void *)es_cubic, (void *)es_linear, (void *)es_step, (void *)es_step_n,
                                                 (void *)es_back, (void *)es_bounce, (void *)es_circle, (void *)es_elastic, (void *)es_exp,
                                                 (void *)es_power, (void *)es_sine };
static const void *const *const g_easing_factory[2] = { easing_factory_vt, easing_statics_vt };
static HRESULT STDMETHODCALLTYPE es_qi(void *p, REFIID iid, void **out)
{
    (void)p;
    if (!out) return E_POINTER;
    *out = 0;
    if (IsEqualGUID(iid, &IID_IUnknown_) || IsEqualGUID(iid, &IID_IInspectable_) || IsEqualGUID(iid, &IID_IAgileObject_) ||
        IsEqualGUID(iid, &IID_IActivationFactory_)) *out = (void *)&g_easing_factory[0];
    else if (IsEqualGUID(iid, &IIDW_CompositionEasingFunctionStatics) || IsEqualGUID(iid, &IIDM_CompositionEasingFunctionStatics))
        *out = (void *)&g_easing_factory[1];
    return *out ? S_OK : E_NOINTERFACE;
}

static BOOL name_is(HSTRING s, const WCHAR *want)
{
    UINT32 n, i = 0;
    const WCHAR *c = WindowsGetStringRawBuffer(s, &n);
    for (; i < n && want[i]; i++) if (c[i] != want[i]) return FALSE;
    return i == n && !want[i];
}
COMP_API HRESULT WINAPI DllGetActivationFactory(HSTRING cls, void **factory)
{
    if (!factory) return E_POINTER;
    /* (the Windows App SDK's Microsoft.UI.Composition classes: the same objects) */
    if (name_is(cls, L"Windows.UI.Composition.Compositor") || name_is(cls, L"Microsoft.UI.Composition.Compositor")) {
        *factory = (void *)&g_factory;
        return S_OK;
    }
    if (name_is(cls, L"Windows.UI.Composition.CompositionEasingFunction") ||
        name_is(cls, L"Microsoft.UI.Composition.CompositionEasingFunction")) {
        *factory = (void *)&g_easing_factory[0];
        return S_OK;
    }
    *factory = 0;
    return CLASS_E_CLASSNOTAVAILABLE;
}
COMP_API HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

/* (dcomp.c: a commit of a compositor's DirectComposition device, after
 * which its surfaces show what was drawn into them) */
void composition_wake(void) { schedule(); }

/* @surface (a DirectComposition surface of dcomp.c's) as an
 * ICompositionSurface of the compositor @compositor (any of its
 * interfaces) for surface brushes, in Windows.UI.Composition's namespace
 * (@ms 0) or Microsoft.UI.Composition's (1): what the XAML framework's
 * composition tree host draws its elements' content into
 * (userland/windows_ui_xaml/src/composition_helper.cpp); NULL: an empty
 * one */
COMP_API HRESULT WINAPI NovaCompositionSurfaceForDComp(IUnknown *compositor, IUnknown *surface, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    Obj *c = 0;
    if (!compositor || FAILED(compositor->lpVtbl->QueryInterface(compositor, &IID_INovaCompositionObject, (void **)&c)) || !c)
        return E_INVALIDARG;
    if (c->kind != K_COMPOSITOR) { obj_release(c); return E_INVALIDARG; }
    Obj *s = obj_new(c, K_CHAIN_SURFACE);
    obj_release(c);
    if (!s) return E_OUTOFMEMORY;
    s->vt[0] = co_vt; s->vt[1] = closable_vt; s->vt[2] = empty2_vt;
    if (surface) { s->dsurf = surface; surface->lpVtbl->AddRef(surface); }
    *out = IF(s, 2);
    return S_OK;
}

/* (and puts another DirectComposition surface, or none, behind one: the
 * XAML framework's surfaces get new pixels after their device was lost,
 * CompositionSurfaceHelper::ResetWinRTSurface) */
COMP_API HRESULT WINAPI NovaCompositionSurfaceSetDComp(IUnknown *winrt, IUnknown *surface)
{
    Obj *s = 0;
    if (!winrt || FAILED(winrt->lpVtbl->QueryInterface(winrt, &IID_INovaCompositionObject, (void **)&s)) || !s)
        return E_INVALIDARG;
    if (s->kind != K_CHAIN_SURFACE) { obj_release(s); return E_INVALIDARG; }
    if (surface) surface->lpVtbl->AddRef(surface);
    if (s->dsurf) s->dsurf->lpVtbl->Release(s->dsurf);
    s->dsurf = surface;
    obj_release(s);
    return S_OK;
}

void composition_init(void)
{
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventW(0, FALSE, FALSE, 0);
}
