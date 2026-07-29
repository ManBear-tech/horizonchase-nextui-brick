/* renderscale.c — render interno em baixa resolução p/ reduzir fill-rate no Mali-450.
 *
 * Diagnóstico (s14): o mapa-múndi roda a ~30fps com só 41 draws/9k verts — não é
 * CPU/animação nem geometria, é FILL-RATE: ~41 quads grandes blended a 1280x720
 * (~1.1 Gpix/s) saturam o Utgard. O menu (7 draws) roda a 60fps. Lever = renderizar a
 * cena numa resolução MENOR e dar upscale p/ os 720p nativos do fb0.
 *
 * Como (GLES2, EGL real do Mali — surface fixa no fb0, sem Screen.SetResolution):
 *  - REDIRECIONA o framebuffer DEFAULT (fb 0 = a tela) p/ um FBO offscreen de baixa res
 *    (g_lo_fbo, ex. 640x360). Toda a renderização "de tela" do Unity cai nele.
 *  - ESCALA o glViewport enquanto a tela (lógico-0) está bound (só aí; os FBOs próprios
 *    do Unity — shadow/RT — passam intactos).
 *  - No eglSwapBuffers: bind o fb 0 REAL, desenha um quad fullscreen amostrando a textura
 *    de cor do FBO lo-res (upscale linear), e então faz o swap real.
 *
 * Gated por CUP_RENDERSCALE=N (divisor; 2 = metade = 640x360). default OFF.
 * Estado salvo/restaurado em volta do blit p/ não corromper o pipeline do Unity.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint;
typedef int GLsizei; typedef unsigned char GLboolean; typedef float GLfloat;
typedef char GLchar; typedef ptrdiff_t GLintptr;

#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_DEPTH_COMPONENT16      0x81A5
#define GL_TEXTURE_2D             0x0DE1
#define GL_RGB                    0x1907
#define GL_RGBA                   0x1908
#define GL_UNSIGNED_BYTE          0x1401
#define GL_LINEAR                 0x2601
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_VERTEX_SHADER          0x8B31
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_ARRAY_BUFFER           0x8892
#define GL_STATIC_DRAW            0x88E4
#define GL_FLOAT                  0x1406
#define GL_FALSE                  0
#define GL_TRUE                   1
#define GL_TRIANGLE_STRIP         0x0005
#define GL_TEXTURE0               0x84C0
#define GL_DEPTH_TEST             0x0B71
#define GL_CULL_FACE              0x0B44
#define GL_BLEND                  0x0BE2
#define GL_SCISSOR_TEST           0x0C11
#define GL_COLOR_BUFFER_BIT       0x4000
#define GL_DEPTH_BUFFER_BIT       0x0100
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#define GL_CURRENT_PROGRAM        0x8B8D
#define GL_ARRAY_BUFFER_BINDING   0x8894
#define GL_ACTIVE_TEXTURE         0x84E7
#define GL_TEXTURE_BINDING_2D     0x8069
#define GL_VIEWPORT               0x0BA2

/* ponteiros p/ as funções GLES2 reais (Mali), resolvidos via dlsym */
static struct {
  void (*GenFramebuffers)(GLsizei, GLuint*);
  void (*BindFramebuffer)(GLenum, GLuint);
  void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
  GLenum (*CheckFramebufferStatus)(GLenum);
  void (*GenRenderbuffers)(GLsizei, GLuint*);
  void (*BindRenderbuffer)(GLenum, GLuint);
  void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
  void (*GenTextures)(GLsizei, GLuint*);
  void (*BindTexture)(GLenum, GLuint);
  void (*TexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
  void (*GetIntegerv)(GLenum, GLint*);
  GLuint (*CreateShader)(GLenum);
  void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
  void (*CompileShader)(GLuint);
  void (*GetShaderiv)(GLuint, GLenum, GLint*);
  GLuint (*CreateProgram)(void);
  void (*AttachShader)(GLuint, GLuint);
  void (*LinkProgram)(GLuint);
  void (*GetProgramiv)(GLuint, GLenum, GLint*);
  void (*UseProgram)(GLuint);
  GLint (*GetAttribLocation)(GLuint, const GLchar*);
  GLint (*GetUniformLocation)(GLuint, const GLchar*);
  void (*GenBuffers)(GLsizei, GLuint*);
  void (*BindBuffer)(GLenum, GLuint);
  void (*BufferData)(GLenum, GLintptr, const void*, GLenum);
  void (*VertexAttribPointer)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);
  void (*EnableVertexAttribArray)(GLuint);
  void (*DisableVertexAttribArray)(GLuint);
  void (*ActiveTexture)(GLenum);
  void (*Uniform1i)(GLint, GLint);
  void (*Disable)(GLenum);
  void (*Enable)(GLenum);
  void (*DrawArrays)(GLenum, GLint, GLsizei);
  void (*Clear)(GLenum);
  void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
  void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
} gl;

static int g_rs_div = 0;            /* divisor (0 = off) */
static int g_scr_w = 1280, g_scr_h = 720;
static int g_lo_w, g_lo_h;
static GLuint g_lo_fbo, g_lo_color, g_lo_depth;
static GLuint g_prog, g_vbo;
static GLint g_a_pos, g_a_uv, g_u_tex;
static int g_logical0 = 1;          /* a tela (fb 0 lógico) está bound? */
static int g_inited = 0;
static int g_in_blit = 0;           /* reentrância: não redirecionar durante o nosso blit */
static unsigned long g_n_bind0, g_n_bindN, g_n_vp, g_n_present;  /* diagnóstico */

static void *gsym(const char *n) { return dlsym(RTLD_DEFAULT, n); }

void rs_init(void) {
  const char *e = getenv("CUP_RENDERSCALE");
  if (!e) return;
  g_rs_div = atoi(e);
  if (g_rs_div < 2) g_rs_div = 2;
  if (getenv("CUP_RS_W")) g_scr_w = atoi(getenv("CUP_RS_W"));
  if (getenv("CUP_RS_H")) g_scr_h = atoi(getenv("CUP_RS_H"));
  g_lo_w = g_scr_w / g_rs_div;
  g_lo_h = g_scr_h / g_rs_div;
  fprintf(stderr, "[RS] render-scale 1/%d -> %dx%d (tela %dx%d)\n",
          g_rs_div, g_lo_w, g_lo_h, g_scr_w, g_scr_h);
}
int rs_enabled(void) { return g_rs_div >= 2; }
int rs_logical0(void) { return g_rs_div >= 2 && g_logical0 && !g_in_blit; }

static const char *VS =
  "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;"
  "void main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0); }";
static const char *FS =
  "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;"
  "void main(){ gl_FragColor=texture2D(uTex,vUV); }";

static GLuint mkshader(GLenum t, const char *src) {
  GLuint s = gl.CreateShader(t);
  gl.ShaderSource(s, 1, &src, 0);
  gl.CompileShader(s);
  GLint ok = 0; gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) fprintf(stderr, "[RS] shader %x falhou compilar\n", t);
  return s;
}

/* cria o FBO lo-res + programa de upscale (precisa de contexto GL current) */
static int rs_lazy_init(void) {
  if (g_inited) return g_lo_fbo != 0;
  g_inited = 1;
  /* resolve todas as funções */
  gl.GenFramebuffers = gsym("glGenFramebuffers");
  gl.BindFramebuffer = gsym("glBindFramebuffer");
  gl.FramebufferTexture2D = gsym("glFramebufferTexture2D");
  gl.FramebufferRenderbuffer = gsym("glFramebufferRenderbuffer");
  gl.CheckFramebufferStatus = gsym("glCheckFramebufferStatus");
  gl.GenRenderbuffers = gsym("glGenRenderbuffers");
  gl.BindRenderbuffer = gsym("glBindRenderbuffer");
  gl.RenderbufferStorage = gsym("glRenderbufferStorage");
  gl.GenTextures = gsym("glGenTextures");
  gl.BindTexture = gsym("glBindTexture");
  gl.TexImage2D = gsym("glTexImage2D");
  gl.TexParameteri = gsym("glTexParameteri");
  gl.Viewport = gsym("glViewport");
  gl.GetIntegerv = gsym("glGetIntegerv");
  gl.CreateShader = gsym("glCreateShader");
  gl.ShaderSource = gsym("glShaderSource");
  gl.CompileShader = gsym("glCompileShader");
  gl.GetShaderiv = gsym("glGetShaderiv");
  gl.CreateProgram = gsym("glCreateProgram");
  gl.AttachShader = gsym("glAttachShader");
  gl.LinkProgram = gsym("glLinkProgram");
  gl.GetProgramiv = gsym("glGetProgramiv");
  gl.UseProgram = gsym("glUseProgram");
  gl.GetAttribLocation = gsym("glGetAttribLocation");
  gl.GetUniformLocation = gsym("glGetUniformLocation");
  gl.GenBuffers = gsym("glGenBuffers");
  gl.BindBuffer = gsym("glBindBuffer");
  gl.BufferData = gsym("glBufferData");
  gl.VertexAttribPointer = gsym("glVertexAttribPointer");
  gl.EnableVertexAttribArray = gsym("glEnableVertexAttribArray");
  gl.DisableVertexAttribArray = gsym("glDisableVertexAttribArray");
  gl.ActiveTexture = gsym("glActiveTexture");
  gl.Uniform1i = gsym("glUniform1i");
  gl.Disable = gsym("glDisable");
  gl.Enable = gsym("glEnable");
  gl.DrawArrays = gsym("glDrawArrays");
  gl.Clear = gsym("glClear");
  if (!gl.GenFramebuffers || !gl.CreateProgram) {
    fprintf(stderr, "[RS] dlsym GLES2 falhou — render-scale OFF\n");
    g_rs_div = 0; return 0;
  }
  GLint prev_fbo = 0, prev_tex = 0, prev_rb = 0;
  gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
  gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  /* textura de cor */
  gl.GenTextures(1, &g_lo_color);
  gl.BindTexture(GL_TEXTURE_2D, g_lo_color);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_lo_w, g_lo_h, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  /* depth renderbuffer */
  gl.GenRenderbuffers(1, &g_lo_depth);
  gl.BindRenderbuffer(GL_RENDERBUFFER, g_lo_depth);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, g_lo_w, g_lo_h);
  /* fbo */
  gl.GenFramebuffers(1, &g_lo_fbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, g_lo_fbo);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_lo_color, 0);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_lo_depth);
  GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE)
    fprintf(stderr, "[RS] FBO incompleto (0x%x) — render-scale pode falhar\n", st);
  /* programa de upscale */
  GLuint vs = mkshader(GL_VERTEX_SHADER, VS), fs = mkshader(GL_FRAGMENT_SHADER, FS);
  g_prog = gl.CreateProgram();
  gl.AttachShader(g_prog, vs); gl.AttachShader(g_prog, fs);
  gl.LinkProgram(g_prog);
  GLint lk = 0; gl.GetProgramiv(g_prog, GL_LINK_STATUS, &lk);
  if (!lk) fprintf(stderr, "[RS] link do upscale falhou\n");
  g_a_pos = gl.GetAttribLocation(g_prog, "aPos");
  g_a_uv  = gl.GetAttribLocation(g_prog, "aUV");
  g_u_tex = gl.GetUniformLocation(g_prog, "uTex");
  /* quad fullscreen (x,y,u,v) triangle strip */
  static const GLfloat quad[] = {
    -1,-1, 0,0,   1,-1, 1,0,   -1,1, 0,1,   1,1, 1,1,
  };
  gl.GenBuffers(1, &g_vbo);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  /* restaura */
  gl.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
  gl.BindBuffer(GL_ARRAY_BUFFER, 0);
  gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
  fprintf(stderr, "[RS] init OK (fbo=%u color=%u prog=%u aPos=%d aUV=%d uTex=%d)\n",
          g_lo_fbo, g_lo_color, g_prog, g_a_pos, g_a_uv, g_u_tex);
  return 1;
}

/* hook de glBindFramebuffer: fb 0 (a tela) -> nosso FBO lo-res */
void rs_BindFramebuffer(GLenum target, GLuint fb) {
  if (!gl.BindFramebuffer) gl.BindFramebuffer = gsym("glBindFramebuffer");
  if (g_in_blit) { gl.BindFramebuffer(target, fb); return; }
  if (!g_inited) rs_lazy_init();
  if (g_rs_div >= 2 && fb == 0) {
    g_n_bind0++;
    g_logical0 = 1;
    gl.BindFramebuffer(target, g_lo_fbo);
    /* o Unity NÃO re-seta o viewport ao voltar p/ a tela (assume full-screen do
     * pass anterior, 1280x720) -> num FBO de 640x360 a cena sai cortada. Força o
     * viewport p/ o tamanho do FBO lo-res. (glViewport posterior do Unity, se houver,
     * passa por rs_Viewport e é escalado corretamente.) */
    if (!gl.Viewport) gl.Viewport = gsym("glViewport");
    gl.Viewport(0, 0, g_lo_w, g_lo_h);
  } else {
    g_n_bindN++;
    g_logical0 = 0;
    gl.BindFramebuffer(target, fb);
  }
}

/* hook de glViewport: escala enquanto a tela lógica está bound */
void rs_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (!gl.Viewport) gl.Viewport = gsym("glViewport");
  if (!g_in_blit && g_rs_div >= 2 && g_logical0) {
    g_n_vp++;
    gl.Viewport(x / g_rs_div, y / g_rs_div, w / g_rs_div, h / g_rs_div);
  } else {
    gl.Viewport(x, y, w, h);
  }
}

/* hook de glScissor: escala enquanto a tela lógica está bound (UI/masking) */
void rs_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (!gl.Scissor) gl.Scissor = gsym("glScissor");
  if (!g_in_blit && g_rs_div >= 2 && g_logical0) {
    gl.Scissor(x / g_rs_div, y / g_rs_div, w / g_rs_div, h / g_rs_div);
  } else {
    gl.Scissor(x, y, w, h);
  }
}

/* chamado ANTES do eglSwapBuffers real: upscale do FBO lo-res p/ a tela real */
void rs_present(void) {
  if (g_rs_div < 2 || !g_inited || !g_lo_fbo) return;
  if (++g_n_present % 120 == 1)
    fprintf(stderr, "[RS] present#%lu bind0=%lu bindN=%lu vp=%lu\n",
            g_n_present, g_n_bind0, g_n_bindN, g_n_vp);
  g_in_blit = 1;
  /* salva estado mínimo que vamos mexer */
  GLint pv[4]; gl.GetIntegerv(GL_VIEWPORT, pv);
  if (g_n_present % 120 == 1)
    fprintf(stderr, "[RS] viewport no present = %d,%d %dx%d (lo=%dx%d)\n",
            pv[0], pv[1], pv[2], pv[3], g_lo_w, g_lo_h);
  GLint pprog = 0, pvbo = 0, ptex = 0, pact = 0;
  gl.GetIntegerv(GL_CURRENT_PROGRAM, &pprog);
  gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &pvbo);
  gl.GetIntegerv(GL_ACTIVE_TEXTURE, &pact);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &ptex);

  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);             /* FBO 0 REAL */
  gl.Viewport(0, 0, g_scr_w, g_scr_h);
  gl.Disable(GL_DEPTH_TEST); gl.Disable(GL_BLEND);
  gl.Disable(GL_CULL_FACE);  gl.Disable(GL_SCISSOR_TEST);
  if (getenv("CUP_RS_DBGCLEAR")) {  /* teste: magenta na tela real antes do quad */
    if (!gl.ClearColor) gl.ClearColor = gsym("glClearColor");
    gl.ClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
  }
  gl.UseProgram(g_prog);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.EnableVertexAttribArray((GLuint)g_a_pos);
  gl.EnableVertexAttribArray((GLuint)g_a_uv);
  gl.VertexAttribPointer((GLuint)g_a_pos, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
  gl.VertexAttribPointer((GLuint)g_a_uv,  2, GL_FLOAT, GL_FALSE, 16, (void*)8);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, g_lo_color);
  gl.Uniform1i(g_u_tex, 0);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  /* restaura */
  gl.DisableVertexAttribArray((GLuint)g_a_pos);
  gl.DisableVertexAttribArray((GLuint)g_a_uv);
  gl.BindTexture(GL_TEXTURE_2D, (GLuint)ptex);
  gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)pvbo);
  gl.UseProgram((GLuint)pprog);
  gl.ActiveTexture((GLuint)pact);
  gl.Viewport(pv[0], pv[1], pv[2], pv[3]);
  /* deixa a tela lógica re-bound p/ o próximo frame do Unity */
  gl.BindFramebuffer(GL_FRAMEBUFFER, g_lo_fbo);
  g_logical0 = 1;
  g_in_blit = 0;
}
