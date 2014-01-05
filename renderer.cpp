/*-------------------------------------------------------------------------
 - mini.q - a minimalistic multiplayer FPS
 - renderer.cpp -> handles rendering routines
 -------------------------------------------------------------------------*/
#include "mini.q.hpp"
#include "iso.hpp"

namespace q {
namespace rr {

void drawdf() {
  ogl::pushmatrix();
  ogl::setortho(-1.f, 1.f, -1.f, 1.f, -1.f, 1.f);
  const float verts[] = {-1.f,-1.f,0.f, 1.f,-1.f,0.f, -1.f,1.f,0.f, 1.f,1.f,0.f};
  ogl::disablev(GL_CULL_FACE, GL_DEPTH_TEST);
  ogl::bindshader(ogl::DFRM_SHADER);
  ogl::immdraw(GL_TRIANGLE_STRIP, 3, 0, 0, 4, verts);
  ogl::enablev(GL_CULL_FACE, GL_DEPTH_TEST);
  ogl::popmatrix();
}

/*--------------------------------------------------------------------------
 - handle the HUD (console, scores...)
 -------------------------------------------------------------------------*/
static void drawhud(int w, int h, int curfps) {
  auto cmd = con::curcmd();
  ogl::pushmode(ogl::MODELVIEW);
  ogl::identity();
  ogl::pushmode(ogl::PROJECTION);
  ogl::setortho(0.f, float(VIRTW), float(VIRTH), 0.f, -1.f, 1.f);
  ogl::enablev(GL_BLEND);
  OGL(DepthMask, GL_FALSE);
  if (cmd) text::drawf("> %s_", 20, 1570, cmd);
  ogl::popmode(ogl::PROJECTION);
  ogl::popmode(ogl::MODELVIEW);

  OGL(DepthMask, GL_TRUE);
  ogl::disable(GL_BLEND);
  ogl::enable(GL_DEPTH_TEST);
}

/*--------------------------------------------------------------------------
 - handle the HUD gun
 -------------------------------------------------------------------------*/
static const char *hudgunnames[] = {
  "hudguns/fist",
  "hudguns/shotg",
  "hudguns/chaing",
  "hudguns/rocket",
  "hudguns/rifle"
};
IVARP(showhudgun, 0, 1, 1);

static void drawhudmodel(int start, int end, float speed, int base) {
  md2::render(hudgunnames[game::player.gun], start, end,
              //game::player.o+vec3f(0.f,0.f,-10.f), game::player.ypr,
              game::player.o+vec3f(0.f,0.f,0.f), game::player.ypr+vec3f(0.f,0.f,0.f),
              //vec3f(zero), game::player.ypr+vec3f(90.f,0.f,0.f),
              false, 1.0f, speed, 0, base);
}

static void drawhudgun(float fovy, float aspect, float farplane) {
  if (!showhudgun) return;

  ogl::enablev(GL_CULL_FACE);
#if 0
  const int rtime = game::reloadtime(game::player.gunselect);
  if (game::player.lastaction &&
      game::player.lastattackgun==game::player.gunselect &&
      game::lastmillis()-game::player.lastaction<rtime)
    drawhudmodel(7, 18, rtime/18.0f, game::player.lastaction);
  else
#endif
  drawhudmodel(6, 1, 100.f, 0);
  ogl::disablev(GL_CULL_FACE);
}

/*--------------------------------------------------------------------------
 - render the complete frame
 -------------------------------------------------------------------------*/
FVARP(fov, 30.f, 90.f, 160.f);

INLINE float U(float d0, float d1) { return min(d0, d1); }
INLINE float D(float d0, float d1) { return max(d0,-d1); }
INLINE float signed_sphere(const vec3f &v, float r) { return length(v) - r; }
INLINE float signed_box(const vec3f &p, const vec3f &b) {
  const vec3f d = abs(p) - b;
  return min(max(d.x,max(d.y,d.z)),0.0f) + length(max(d,vec3f(zero)));
}
INLINE float signed_cyl(const vec3f &p, const vec2f &cxz, float r) {
  return length(p.xz()-cxz) - r;
}
INLINE float signed_plane(const vec3f &p, const vec4f &n) {
  return dot(p,n.xyz())+n.w;
}
INLINE float signed_cyl(const vec3f &p, const vec2f &cxz, const vec3f &ryminymax) {
  const auto cyl = signed_cyl(p, cxz, ryminymax.x);
  const auto plane0 = signed_plane(p, vec4f(0.f,1.f,0.f,ryminymax.y));
  const auto plane1 = signed_plane(p, vec4f(0.f,-1.f,0.f,ryminymax.z));
  return D(D(cyl, plane0), plane1);
}

static float map(const vec3f &pos) {
  const auto t = pos-vec3f(7.f,5.f,7.f);
  const auto d0 = signed_sphere(t, 4.2f);
  const auto d1 = signed_box(t, vec3f(4.f));
  const auto c = signed_cyl(pos, vec2f(2.f,2.f), vec3f(1.f,0.f,1.f));
  return U(D(d1, d0), c);
}

typedef pair<vec3f,vec3f> vertex;
static u32 vertnum = 0u, indexnum = 0u;
static vector<vertex> vertices;
static vector<u32> indices;
static bool initialized_m = false;

void start() {}
void finish() {
  vector<vertex>().moveto(vertices);
  vector<u32>().moveto(indices);
}

static const float cellsize = 0.1f;
static const int griddim = 16;
static void makescene() {
  if (initialized_m) return;
  const vec3f dim(float(griddim) * cellsize);
  const float start = sys::millis();
  auto m = iso::dc_mesh(vec3f(zero), 256, cellsize, map);
  loopi(m.m_vertnum) vertices.add(makepair(m.m_pos[i], m.m_nor[i]));
  loopi(m.m_indexnum) indices.add(m.m_index[i]);
  vertnum = m.m_vertnum;
  indexnum = m.m_indexnum;
  con::out("elapsed %f ms", sys::millis() - start);
  con::out("tris %i verts %i", indexnum/3, vertnum);
  initialized_m = true;
}

static void transplayer(void) {
  using namespace game;
  ogl::identity();
  ogl::rotate(player.ypr.z, vec3f(0.f,0.f,1.f));
  ogl::rotate(player.ypr.y, vec3f(1.f,0.f,0.f));
  ogl::rotate(player.ypr.x, vec3f(0.f,1.f,0.f));
  ogl::translate(-player.o);
}

void frame(int w, int h, int curfps) {
  const float farplane = 100.f;
  const float fovy = fov * float(sys::scrh) / float(sys::scrw);
  const float aspect = float(sys::scrw) / float(sys::scrh);
  OGL(ClearColor, 0.f, 0.f, 0.f, 1.f);
  OGL(Clear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  ogl::matrixmode(ogl::PROJECTION);
  ogl::setperspective(fovy, aspect, 0.15f, farplane);
  ogl::matrixmode(ogl::MODELVIEW);
  transplayer();
  const float verts[] = {
    -100.f, -100.f, -100.f, 0.f, -100.f,
    +100.f, -100.f, +100.f, 0.f, -100.f,
    -100.f, +100.f, -100.f, 0.f, +100.f,
    +100.f, +100.f, +100.f, 0.f, +100.f
  };
  ogl::bindfixedshader(ogl::DIFFUSETEX);
  ogl::bindtexture(GL_TEXTURE_2D, ogl::coretex(ogl::TEX_CHECKBOARD));
  ogl::immdraw(GL_TRIANGLE_STRIP, 3, 2, 0, 4, verts);

  makescene();
  if (vertnum != 0) {
    OGL(PolygonMode, GL_FRONT_AND_BACK, GL_LINE);
    ogl::bindfixedshader(ogl::COLOR);
    ogl::immdrawelememts("Tip3c3", indexnum, &indices[0], &vertices[0].first[0]);
    OGL(PolygonMode, GL_FRONT_AND_BACK, GL_FILL);
  }

  drawhud(w,h,0);
  drawhudgun(fovy, aspect, farplane);
}
} /* namespace rr */
} /* namespace q */

