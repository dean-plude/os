/* hlsltest — NovaOS's HLSL compiler (d3dcompiler_47.dll, on vkd3d-shader)
 * the way Chromium's ANGLE and games use it: HLSL compiled to Direct3D
 * bytecode with D3DCompile (vs/ps 4_0 and 5_0, as ANGLE asks for them, and
 * ps_3_0), errors, the preprocessor and #include, reflection, the
 * disassembler and the container parts, then the shaders drawn with
 * Direct3D 11 (DXVK): a triangle whose colour comes from a texture, a
 * constant buffer, a function with an out parameter and a loop, read back.
 * Build: x86_64-w64-mingw32-gcc -O2 -o hlsltest.exe hlsltest.c -ld3dcompiler_47 -ld3d11 -ldxguid
 *        (i686-w64-mingw32-gcc for the 32-bit one).  Usage: hlsltest */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the IID d3dcompiler_47 answers to in the Windows 10 SDK */
/* MinGW's d3d11shader.h has no C macros for these: call through the vtable */
#define CALL(o, m, ...) ((o)->lpVtbl->m((o), ##__VA_ARGS__))
#define SHVER_TYPE(v) ((v) >> 16 & 0xffff)
#define SHVER_MAJOR(v) ((v) >> 4 & 0xf)
#define SHVER_MINOR(v) ((v) & 0xf)

DEFINE_GUID(IID_ID3D11ShaderReflection_47, 0x8d536ca1, 0x0cca, 0x4956, 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84);

static int pass, fail;
static void check(const char *what, int ok) { if (ok) pass++; else { fail++; printf("FAIL %s\n", what); } }

static const char shader[] =
    "cbuffer Params : register(b0) { float4 tint; float2 offset; float scale; };\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(VSIn i)\n"
    "{\n"
    "    VSOut o;\n"
    "    o.pos = float4(i.pos * scale + offset, 0.5, 1.0);\n"
    "    o.uv = i.uv;\n"
    "    return o;\n"
    "}\n"
    "void mix_colour(in float4 a, in float4 b, out float4 r) { r = lerp(a, b, 0.5); }\n"
    "float4 ps_main(VSOut i) : SV_Target\n"
    "{\n"
    "    float4 t = tex.Sample(smp, i.uv);\n"
    "    float4 r;\n"
    "    mix_colour(t, tint, r);\n"
    "    float s = 0.0;\n"
    "    [loop] for (int k = 0; k < 4; k++) s += 0.25;\n"
    "    return float4(r.rgb * s, 1.0);\n"
    "}\n";

static ID3DBlob *compile(const char *src, const char *entry, const char *profile, HRESULT *hr_out, ID3DBlob **errors)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "hlsltest.hlsl", NULL, NULL, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (hr_out) *hr_out = hr;
    if (FAILED(hr)) printf("%s %s: 0x%08lx %s\n", entry, profile, (unsigned long)hr,
                           err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
    if (errors) *errors = err;
    else if (err) ID3D10Blob_Release(err);
    return code;
}

static int is_dxbc(ID3DBlob *b)
{
    return b && ID3D10Blob_GetBufferSize(b) > 32 && !memcmp(ID3D10Blob_GetBufferPointer(b), "DXBC", 4);
}

/* ---- an ID3DInclude that serves one header from memory ---- */
static HRESULT STDMETHODCALLTYPE inc_open(ID3DInclude *self, D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent,
                                          LPCVOID *data, UINT *bytes)
{
    static const char header[] = "#define HALF 0.5\nfloat4 halve(float4 c) { return c * HALF; }\n";
    (void)self; (void)type; (void)parent;
    if (strcmp(name, "common.hlsli")) return E_FAIL;
    *data = header;
    *bytes = sizeof(header) - 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE inc_close(ID3DInclude *self, LPCVOID data) { (void)self; (void)data; return S_OK; }
static ID3DIncludeVtbl inc_vtbl = { inc_open, inc_close };
static ID3DInclude inc = { &inc_vtbl };

static void test_compiler(void)
{
    HRESULT hr;
    ID3DBlob *err = NULL;

    /* the profiles ANGLE compiles its shaders for, and Direct3D 9's */
    const char *profiles[][2] = { { "vs_main", "vs_4_0" }, { "ps_main", "ps_4_0" }, { "vs_main", "vs_5_0" },
                                  { "ps_main", "ps_5_0" } };
    for (int i = 0; i < 4; i++) {
        char what[64];
        ID3DBlob *b = compile(shader, profiles[i][0], profiles[i][1], &hr, NULL);
        snprintf(what, sizeof(what), "D3DCompile %s", profiles[i][1]);
        check(what, SUCCEEDED(hr) && is_dxbc(b));
        if (b) ID3D10Blob_Release(b);
    }
    ID3DBlob *sm3 = compile("float4 c; float4 main() : COLOR { return c * 2; }", "main", "ps_3_0", &hr, NULL);
    check("D3DCompile ps_3_0", SUCCEEDED(hr) && sm3 && ID3D10Blob_GetBufferSize(sm3) >= 4 &&
          *(DWORD *)ID3D10Blob_GetBufferPointer(sm3) == 0xFFFF0300);
    if (sm3) ID3D10Blob_Release(sm3);

    /* a mistake: E_FAIL, no code, and the compiler's message */
    ID3DBlob *bad = compile("float4 main() : SV_Target { return undefined_thing; }", "main", "ps_4_0", &hr, &err);
    check("D3DCompile reports errors", hr == E_FAIL && !bad && err &&
          strstr((const char *)ID3D10Blob_GetBufferPointer(err), "undefined_thing"));
    if (err) { printf("error message  %s", (const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }

    /* #include through an ID3DInclude, and macros */
    static const char with_include[] = "#include \"common.hlsli\"\nfloat4 main() : SV_Target { return halve(COLOUR); }\n";
    D3D_SHADER_MACRO defs[] = { { "COLOUR", "float4(1, 0, 0, 1)" }, { NULL, NULL } };
    ID3DBlob *code = NULL;
    hr = D3DCompile(with_include, strlen(with_include), "inc.hlsl", defs, &inc, "main", "ps_4_0", 0, 0, &code, &err);
    check("D3DCompile with #include and macros", SUCCEEDED(hr) && is_dxbc(code));
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    code = err = NULL;
    hr = D3DPreprocess(with_include, strlen(with_include), "inc.hlsl", defs, &inc, &code, &err);
    char flat[512] = "";                                /* the text without its spaces */
    if (code) {
        const char *t = ID3D10Blob_GetBufferPointer(code);
        size_t k = 0;
        for (SIZE_T j = 0; j < ID3D10Blob_GetBufferSize(code) && t[j] && k < sizeof(flat) - 1; j++)
            if (t[j] != ' ' && t[j] != '\t' && t[j] != '\n' && t[j] != '\r') flat[k++] = t[j];
        flat[k] = 0;
    }
    check("D3DPreprocess", SUCCEEDED(hr) && strstr(flat, "returnhalve(float4(1,0,0,1));") && strstr(flat, "returnc*0.5;"));
    if (!strstr(flat, "returnhalve")) printf("preprocessed  %s\n", flat);
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);

    /* reflection of the pixel and vertex shaders */
    ID3DBlob *ps = compile(shader, "ps_main", "ps_5_0", NULL, NULL);
    ID3DBlob *vs = compile(shader, "vs_main", "vs_5_0", NULL, NULL);
    if (!ps || !vs) { check("shaders for reflection", 0); return; }
    ID3D11ShaderReflection *r = NULL;
    hr = D3DReflect(ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), &IID_ID3D11ShaderReflection_47, (void **)&r);
    check("D3DReflect", SUCCEEDED(hr) && r);
    if (r) {
        D3D11_SHADER_DESC d;
        hr = CALL(r, GetDesc, &d);
        printf("reflection  version %#x  cbuffers %u  resources %u  in %u  out %u  instructions %u\n", d.Version,
               d.ConstantBuffers, d.BoundResources, d.InputParameters, d.OutputParameters, d.InstructionCount);
        check("reflection: pixel shader 5.0", SUCCEEDED(hr) && SHVER_TYPE(d.Version) == 0 &&
              SHVER_MAJOR(d.Version) == 5 && SHVER_MINOR(d.Version) == 0);
        check("reflection: counts", d.ConstantBuffers == 1 && d.BoundResources == 3 && d.InputParameters == 2 &&
              d.OutputParameters == 1 && d.InstructionCount > 0);
        D3D11_SHADER_INPUT_BIND_DESC bd;
        check("reflection: the texture's binding", SUCCEEDED(CALL(r, GetResourceBindingDescByName, "tex", &bd)) &&
              bd.Type == D3D_SIT_TEXTURE && bd.BindPoint == 0 && bd.Dimension == D3D_SRV_DIMENSION_TEXTURE2D);
        ID3D11ShaderReflectionConstantBuffer *cb = CALL(r, GetConstantBufferByName, "Params");
        D3D11_SHADER_BUFFER_DESC cbd;
        check("reflection: the constant buffer", SUCCEEDED(CALL(cb, GetDesc, &cbd)) &&
              !strcmp(cbd.Name, "Params") && cbd.Variables == 3 && cbd.Size == 32);
        ID3D11ShaderReflectionVariable *v = CALL(cb, GetVariableByName, "scale");
        D3D11_SHADER_VARIABLE_DESC vd;
        D3D11_SHADER_TYPE_DESC td;
        check("reflection: a variable", SUCCEEDED(CALL(v, GetDesc, &vd)) && vd.StartOffset == 24 &&
              vd.Size == 4);
        ID3D11ShaderReflectionType *t = CALL(v, GetType);
        check("reflection: its type", SUCCEEDED(CALL(t, GetDesc, &td)) && td.Class == D3D_SVC_SCALAR &&
              td.Type == D3D_SVT_FLOAT && td.Rows == 1 && td.Columns == 1);
        ID3D11ShaderReflectionVariable *none = CALL(r, GetVariableByName, "no_such_variable");
        check("reflection: an unknown name", none && FAILED(CALL(none, GetDesc, &vd)));
        D3D11_SIGNATURE_PARAMETER_DESC pd;
        check("reflection: the output", SUCCEEDED(CALL(r, GetOutputParameterDesc, 0, &pd)) &&
              !strcmp(pd.SemanticName, "SV_Target") && pd.SystemValueType == D3D_NAME_TARGET);
        UINT level = 0;
        check("reflection: feature level", SUCCEEDED(CALL(r, GetMinFeatureLevel, (D3D_FEATURE_LEVEL *)&level)) &&
              level == D3D_FEATURE_LEVEL_11_0);
        CALL(r, Release);
    }
    r = NULL;
    hr = D3DReflect(ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &IID_ID3D11ShaderReflection, (void **)&r);
    if (r) {
        D3D11_SIGNATURE_PARAMETER_DESC pd;
        check("reflection: the vertex shader's input", SUCCEEDED(CALL(r, GetInputParameterDesc, 0, &pd)) &&
              !strcmp(pd.SemanticName, "POSITION") && pd.Register == 0 && pd.Mask == 3 &&
              pd.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32);
        CALL(r, Release);
    } else {
        check("D3DReflect (vertex shader)", 0);
    }

    /* the disassembler and the container's parts */
    ID3DBlob *text = NULL;
    hr = D3DDisassemble(ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), 0, NULL, &text);
    check("D3DDisassemble", SUCCEEDED(hr) && text && strstr((const char *)ID3D10Blob_GetBufferPointer(text), "ps_5_0") &&
          strstr((const char *)ID3D10Blob_GetBufferPointer(text), "sample"));
    if (text) ID3D10Blob_Release(text);
    ID3DBlob *sig = NULL;
    hr = D3DGetInputSignatureBlob(ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &sig);
    check("D3DGetInputSignatureBlob", SUCCEEDED(hr) && is_dxbc(sig) && ID3D10Blob_GetBufferSize(sig) < ID3D10Blob_GetBufferSize(vs));
    if (sig) ID3D10Blob_Release(sig);
    sig = NULL;
    hr = D3DGetBlobPart(ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), D3D_BLOB_DEBUG_INFO, 0, &sig);
    check("D3DGetBlobPart: no debug information", hr == E_FAIL && !sig);
    ID3DBlob *stripped = NULL;
    hr = D3DStripShader(ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped);
    check("D3DStripShader", SUCCEEDED(hr) && is_dxbc(stripped) && ID3D10Blob_GetBufferSize(stripped) < ID3D10Blob_GetBufferSize(ps));
    if (stripped) ID3D10Blob_Release(stripped);
    ID3D10Blob_Release(ps);
    ID3D10Blob_Release(vs);
}

/* ---- the shaders on Direct3D 11 ---- */
static int px_near(const BYTE *p, int r, int g, int b)          /* p: R8G8B8A8 */
{
    return abs(p[0] - r) < 8 && abs(p[1] - g) < 8 && abs(p[2] - b) < 8;
}

static void draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, const char *vsp, const char *psp)
{
    char what[96];
    ID3DBlob *vsb = compile(shader, "vs_main", vsp, NULL, NULL), *psb = compile(shader, "ps_main", psp, NULL, NULL);
    if (!vsb || !psb) { snprintf(what, sizeof(what), "%s/%s compiled", vsp, psp); check(what, 0); return; }
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *il = NULL;
    HRESULT hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    snprintf(what, sizeof(what), "CreateVertexShader (%s)", vsp);
    check(what, SUCCEEDED(hr));
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    snprintf(what, sizeof(what), "CreatePixelShader (%s)", psp);
    check(what, SUCCEEDED(hr));
    D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = ID3D11Device_CreateInputLayout(dev, elems, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &il);
    snprintf(what, sizeof(what), "CreateInputLayout (%s signature)", vsp);
    check(what, SUCCEEDED(hr));

    /* the vertices, the constants (a red tint, no offset, scale 1) and a green texel */
    float verts[] = { -0.8f, -0.8f, 0, 1,   0.0f, 0.8f, 0.5f, 0,   0.8f, -0.8f, 1, 1 };
    float params[8] = { 1, 0, 0, 1,   0, 0,   1, 0 };
    DWORD texel = 0xFF00FF00;                           /* R8G8B8A8: green */
    D3D11_BUFFER_DESC vbd = { sizeof(verts), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_BUFFER_DESC cbd = { sizeof(params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA vinit = { verts, 0, 0 }, cinit = { params, 0, 0 }, tinit = { &texel, 4, 0 };
    ID3D11Buffer *vb = NULL, *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &vbd, &vinit, &vb);
    ID3D11Device_CreateBuffer(dev, &cbd, &cinit, &cb);
    D3D11_TEXTURE2D_DESC td = { 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, &tinit, &tex);
    if (tex) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv);
    D3D11_SAMPLER_DESC sd = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, { 0, 0, 0, 0 }, 0, D3D11_FLOAT32_MAX };
    ID3D11SamplerState *smp = NULL;
    ID3D11Device_CreateSamplerState(dev, &sd, &smp);

    /* a 64x64 target, cleared blue */
    D3D11_TEXTURE2D_DESC rd = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *rt = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateTexture2D(dev, &rd, NULL, &rt);
    if (rt) ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    rd.Usage = D3D11_USAGE_STAGING;
    rd.BindFlags = 0;
    rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &rd, NULL, &staging);
    if (!vs || !ps || !il || !vb || !cb || !srv || !smp || !rtv || !staging) {
        check("Direct3D 11 objects", 0);
        goto done;
    }
    float blue[4] = { 0, 0, 1, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, blue);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    UINT stride = 16, offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m);
    snprintf(what, sizeof(what), "read back (%s/%s)", vsp, psp);
    check(what, SUCCEEDED(hr));
    if (SUCCEEDED(hr)) {
        const BYTE *corner = (const BYTE *)m.pData + 2 * m.RowPitch + 2 * 4;
        const BYTE *mid = (const BYTE *)m.pData + 36 * m.RowPitch + 32 * 4;
        printf("D3D11 pixels (%s/%s)  corner %02x%02x%02x  centre %02x%02x%02x\n", vsp, psp,
               corner[0], corner[1], corner[2], mid[0], mid[1], mid[2]);
        snprintf(what, sizeof(what), "clear colour (%s/%s)", vsp, psp);
        check(what, px_near(corner, 0, 0, 255));
        snprintf(what, sizeof(what), "shaded triangle (%s/%s)", vsp, psp);
        check(what, px_near(mid, 128, 128, 0));            /* half green texel, half red tint, times the loop's 1 */
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
done:
    ID3D11DeviceContext_ClearState(ctx);
    if (staging) ID3D11Texture2D_Release(staging);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (rt) ID3D11Texture2D_Release(rt);
    if (smp) ID3D11SamplerState_Release(smp);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (tex) ID3D11Texture2D_Release(tex);
    if (cb) ID3D11Buffer_Release(cb);
    if (vb) ID3D11Buffer_Release(vb);
    if (il) ID3D11InputLayout_Release(il);
    if (ps) ID3D11PixelShader_Release(ps);
    if (vs) ID3D11VertexShader_Release(vs);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
}

static void test_d3d11(void)
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got = 0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 3, D3D11_SDK_VERSION, &dev, &got, &ctx);
    check("D3D11CreateDevice", SUCCEEDED(hr) && dev && ctx);
    if (!dev) { printf("D3D11CreateDevice: 0x%08lx\n", (unsigned long)hr); return; }
    printf("D3D11 feature level %#x\n", got);
    draw(dev, ctx, "vs_4_0", "ps_4_0");
    if (got >= D3D_FEATURE_LEVEL_11_0) draw(dev, ctx, "vs_5_0", "ps_5_0");
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
}

int main(int argc, char **argv)
{
    (void)argv;
    test_compiler();
    if (argc < 2) test_d3d11();                         /* hlsltest nodevice: the compiler only */
    printf("hlsltest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
