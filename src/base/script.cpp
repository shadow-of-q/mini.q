/*-------------------------------------------------------------------------
 - mini.q - a minimalistic multiplayer FPS
 - script.cpp -> implements embedded script / con language
 -------------------------------------------------------------------------*/
#include "sys.hpp"
#include "script.hpp"
#include "client.hpp"
#include "console.hpp"
#include "hash_map.hpp"
#include "base/lua/lualib.h"
#include <cstdio>

namespace q {
namespace script {

enum { ID_VAR, ID_CMD, ID_ALIAS };

static void *internalalloc(void*, void *ptr, size_t, size_t nsize) {
  return sys::memrealloc(ptr, nsize, __FILE__, __LINE__);
}

lua_State *luastate() {
  static lua_State *L = NULL;
  if (L == NULL) {
    L = lua_newstate(internalalloc, NULL);
    luaL_openlibs(L);
  }
  return L;
}

struct identifier {
  int type; // one of ID_* above
  const char *name;
  int min, max; // ID_VAR
  int *storage; // ID_VAR
  void (*fun)(); // ID_VAR, ID_CMD
  int narg; // ID_VAR, ID_CMD
  char *action; // ID_ALIAS
  bool persist;
};

// tab-completion of all idents
static int completesize = 0, completeidx = 0;

static void itoa(fixedstring &s, int i) { s.fmt("%d", i); }
static char *exchangestr(char *o, const char *n) {
  FREE(o);
  return NEWSTRING(n);
}

// contains all vars/commands/aliases
typedef hash_map<string_ref,identifier> identifier_map;
static identifier_map *idents = NULL;
static identifier *access(const char *name) {
  const auto it = idents->find(name);
  if (it != idents->end())
    return &it->second;
  else
    return NULL;
}

void finish(void) {
  lua_close(luastate());
  for (auto it = idents->begin(); it != idents->end(); ++it) {
    if (it->second.type!=ID_ALIAS) continue;
    FREE((char*) it->second.name);
    FREE((char*) it->second.action);
  }
  SAFE_DEL(idents);
  idents = NULL;
}

static void initializeidents(void) {
  if (!idents) idents = NEWE(identifier_map);
}

void alias(const char *name, const char *action) {
  auto b = idents->find(name);
  if (b == idents->end()) {
    name = NEWSTRING(name);
    const identifier b = {ID_ALIAS, name, 0, 0, 0, 0, 0, NEWSTRING(action), true};
    idents->insert(makepair(name, b));
  } else {
    if (b->second.type==ID_ALIAS)
      b->second.action = exchangestr(b->second.action, action);
    else
      con::out("cannot redefine builtin %s with an alias", name);
  }
}
CMD(alias, ARG_2STR);

int variable(const char *name, int min, int cur, int max,
             int *storage, void (*fun)(), bool persist) {
  initializeidents();
  const identifier v = {ID_VAR, name, min, max, storage, fun, 0, 0, persist};
  idents->insert(makepair(name, v));
  luabridge::getGlobalNamespace(luastate())
    .beginNamespace("var")
    .addVariable(name, storage, min, max, fun)
    .endNamespace();
  return cur;
}

void setvar(const char *name, int i) {
  const auto it = idents->find(name);
  assert(it != idents->end());
  *it->second.storage = i;
}
int getvar(const char *name) {
  const auto it = idents->find(name);
  assert(it != idents->end());
  return *it->second.storage;
}
bool identexists(const char *name) {
  return idents->find(name) != idents->end();
}

char *getalias(const char *name) {
  const auto it = idents->find(name);
  return (it != idents->end() && it->second.type==ID_ALIAS) ?
    it->second.action : NULL;
}

bool addcommand(const char *name, void (*fun)(), int narg) {
  initializeidents();
  const identifier c = { ID_CMD, name, 0, 0, 0, fun, narg, 0, false };
  idents->insert(makepair(name, c));
  return false;
}

static int luareport(int ret) {
  auto L = luastate();
  if (ret && !lua_isnil(L, -1)) {
    const char *msg = lua_tostring(L, -1);
    if (msg != NULL) con::out("lua script failed with %s", msg);
    lua_pop(L, 1);
  }
  return ret;
}

int executelua(const char *p) {
  auto L = luastate();
  if (luareport(luaL_loadstring(L, p))) return 0;
  //lua_getglobal(L, "_G");
  //lua_setupvalue(L, -2, 1);
  return luareport(lua_pcall(L, 0, 0, 0));
}

bool execluascript(const char *cfgfile) {
  fixedstring s(cfgfile);
  const auto buf = sys::loadfile(sys::path(s.c_str()), NULL);
  if (!buf) return false;
  executelua(buf);
  FREE(buf);
  return true;
}

void execscript(const char *cfgfile) {
  if (!execluascript(cfgfile))
    con::out("could not read \"%s\"", cfgfile);
}
CMD(execscript, ARG_1STR);

// parse any nested set of () or []
static char *parseexp(char *&p, int right) {
  int left = *p++;
  const char *word = p;
  for (int brak = 1; brak; ) {
    const int c = *p++;
    if (c=='\r') *(p-1) = ' ';  // hack
    if (c==left) brak++;
    else if (c==right) brak--;
    else if (!c) { p--; con::out("missing \"%c\"", right); return NULL; }
  }
  char *s = NEWSTRING(word, p-word-1);
  if (left=='(') {
    fixedstring t;
    itoa(t, execstring(s)); // evaluate () exps directly, and substitute result
    s = exchangestr(s, t.c_str());
  }
  return s;
}

// parse single argument, including expressions
static char *parseword(char *&p) {
  p += strspn(p, " \t\r");
  if (p[0]=='/' && p[1]=='/') p += strcspn(p, "\n\0");
  if (*p=='\"') {
    p++;
    const char *word = p;
    p += strcspn(p, "\"\r\n\0");
    char *s = NEWSTRING(word, p-word);
    if (*p=='\"') p++;
    return s;
  }
  if (*p=='(') return parseexp(p, ')');
  if (*p=='[') return parseexp(p, ']');
  char *word = p;
  p += strcspn(p, "; \t\r\n\0");
  if (p-word==0) return NULL;
  return NEWSTRING(word, p-word);
}

// find value of ident referenced with $ in exp
static char *lookup(char *n) {
  const auto id = access(n+1);
  if (id) switch (id->type) {
    case ID_VAR: {
      fixedstring t;
      itoa(t, *(id->storage));
      return exchangestr(n, t.c_str());
    }
    case ID_ALIAS: return exchangestr(n, id->action);
  }
  con::out("unknown alias lookup: %s", n+1);
  return n;
}

int execstring(const char *pp, bool isdown) {
  char *p = (char*)pp;
  const int MAXWORDS = 25; // limit, remove
  char *w[MAXWORDS];
  int val = 0;
  for (bool cont = true; cont;) { // for each ; seperated statement
    int numargs = MAXWORDS;
    loopi(MAXWORDS) { // collect all argument values
      w[i] = (char*) "";
      if (i>numargs) continue;
      char *s = parseword(p); // parse and evaluate exps
      if (!s) {
        numargs = i;
        s = (char*) "";
      }
      if (*s=='$') s = lookup(s); // substitute variables
      w[i] = s;
    }

    p += strcspn(p, ";\n\0");
    cont = *p++!=0; // more statements if this isn't the end of the string
    const char *c = w[0];
    if (*c=='/') c++;  // strip irc-style command prefix
    if (!*c) {
      loopj(numargs) FREE(w[j]);
      continue;  // empty statement
    }

    const auto id = access(c);
    if (!id) {
      val = ATOI(c);
      if (!val && *c!='0')
        con::out("unknown command: %s", c);
    } else {
      switch (id->type) {
        // game defined command
        case ID_CMD:
          switch (id->narg) { // use very ad-hoc function signature, and call it
            case ARG_1INT:
              if (isdown) ((void (CDECL *)(int))id->fun)(ATOI(w[1])); break;
            case ARG_2INT:
              if (isdown) ((void (CDECL *)(int, int))id->fun)(ATOI(w[1]), ATOI(w[2])); break;
            case ARG_3INT:
              if (isdown) ((void (CDECL *)(int, int, int))id->fun)(ATOI(w[1]), ATOI(w[2]), ATOI(w[3])); break;
            case ARG_4INT:
              if (isdown) ((void (CDECL *)(int, int, int, int))id->fun)(ATOI(w[1]), ATOI(w[2]), ATOI(w[3]), ATOI(w[4])); break;
            case ARG_NONE:
              if (isdown) ((void (CDECL *)())id->fun)(); break;
            case ARG_1STR:
              if (isdown) ((void (CDECL *)(char *))id->fun)(w[1]); break;
            case ARG_2STR:
              if (isdown) ((void (CDECL *)(char *, char *))id->fun)(w[1], w[2]); break;
            case ARG_3STR:
              if (isdown) ((void (CDECL *)(char *, char *, char*))id->fun)(w[1], w[2], w[3]); break;
            case ARG_5STR:
              if (isdown) ((void (CDECL *)(char *, char *, char*, char*, char*))id->fun)(w[1], w[2], w[3], w[4], w[5]); break;
            case ARG_DOWN:
              ((void (CDECL *)(bool))id->fun)(isdown); break;
            case ARG_DWN1:
              ((void (CDECL *)(bool, char *))id->fun)(isdown, w[1]); break;
            case ARG_1EXP:
              if (isdown) val = ((int (CDECL *)(int))id->fun)(execstring(w[1])); break;
            case ARG_2EXP:
              if (isdown) val = ((int (CDECL *)(int, int))id->fun)(execstring(w[1]), execstring(w[2])); break;
            case ARG_1EST:
              if (isdown) val = ((int (CDECL *)(char *))id->fun)(w[1]); break;
            case ARG_2EST:
              if (isdown) val = ((int (CDECL *)(char *, char *))id->fun)(w[1], w[2]); break;
            case ARG_VARI:
              if (isdown) {
                fixedstring r; // limit, remove
                r[0] = 0;
                for (int i = 1; i<numargs; i++) {
                  strcat_s(r, w[i]);  // make string-list out of all arguments
                  if (i==numargs-1) break;
                  strcat_s(r, " ");
                }
                ((void (CDECL *)(char *))id->fun)(r.c_str());
                break;
              }
          }
        break;

        // game defined variable
        case ID_VAR:
          if (isdown) {
            if (!w[1][0])
              // var with no value just prints its current value
              con::out("%s = %d", c, *id->storage); 
            else {
              if (id->min>id->max)
                con::out("variable is read-only");
              else {
                int i1 = ATOI(w[1]);
                if (i1<id->min || i1>id->max) {
                  i1 = i1<id->min ? id->min : id->max; // clamp to valid range
                  con::out("valid range for %s is %d..%d", c, id->min, id->max);
                }
                *id->storage = i1;
              }
              // call trigger function if available
              if (id->fun) ((void (CDECL *)())id->fun)(); 
            }
          }
        break;

        // alias, also used as functions and (global) variables
        case ID_ALIAS:
          for (int i = 1; i<numargs; i++) {
            // set any arguments as (global) arg values so functions can access them
            fixedstring t(fmt, "arg%d", i);
            alias(t.c_str(), w[i]);
          }
          // create new string here because alias could rebind itself
          char *action = NEWSTRING(id->action);
          val = execstring(action, isdown);
          FREE(action);
        break;
      }
    }
    loopj(numargs) FREE(w[j]);
  }
  return val;
}

void resetcomplete(void) { completesize = 0; }

void complete(fixedstring &s) {
  if (*s.c_str()!='/') {
    fixedstring t;
    strcpy_s(t, s.c_str());
    strcpy_s(s, "/");
    strcat_s(s, t.c_str());
  }
  if (!s[1]) return;
  if (!completesize) {
    completesize = int(strlen(s.c_str())-1);
    completeidx = 0;
  }
  int idx = 0;
  for (auto it = idents->begin(); it != idents->end(); ++it)
    if (strncmp(it->first.c_str(), s.c_str()+1, completesize)==0 && idx++==completeidx)
      s.fmt("/%s", it->first.c_str());
  completeidx++;
  if (completeidx>=idx) completeidx = 0;
}

bool execfile(const char *cfgfile) {
  fixedstring s;
  strcpy_s(s, cfgfile);
  char *buf = sys::loadfile(sys::path(s.c_str()), NULL);
  if (!buf) return false;
  execstring(buf);
  FREE(buf);
  return true;
}

void exec(const char *cfgfile) {
  if (!execfile(cfgfile))
    con::out("could not read \"%s\"", cfgfile);
}

void writecfg(void) {
  auto f = fopen("config.q", "w");
  if (!f) return;
  fprintf(f, "// automatically written on exit, do not modify\n"
             "// delete this file to have defaults.cfg overwrite these settings\n"
             "// modify settings in game, or put settings in autoexec.cfg to override anything\n\n");
  for (auto it = idents->begin(); it != idents->end(); ++it)
    if (it->second.persist)
      fprintf(f, "%s %d\n", it->first.c_str(), *it->second.storage);
  fclose(f);
}

CMD(writecfg, ARG_NONE);

// below the commands that implement a small imperative language. thanks to the
// semantics of () and [] expressions, any control construct can be defined
// trivially.
static void intset(const char *name, int v) { fixedstring b; itoa(b, v); alias(name, b.c_str()); }
static void ifthen(char *cond, char *thenp, char *elsep) { execstring(cond[0]!='0' ? thenp : elsep); }
static void loopa(char *times, char *body) { int t = atoi(times); loopi(t) { intset("i", i); execstring(body); } }
static void whilea(char *cond, char *body) { while (execstring(cond)) execstring(body); } // can't get any simpler than this :)
static void onrelease(bool on, char *body) { if (!on) execstring(body); }
static void concat(char *s) { alias("s", s); }
static void concatword(char *s) {
  for (char *a = s, *b = s; (*a = *b) != 0; b++) if (*a!=' ') a++;
  concat(s);
}

static int listlen(char *a) {
  if (!*a) return 0;
  int n = 0;
  while (*a) if (*a++==' ') n++;
  return n+1;
}

static void at(char *s, char *pos) {
  int n = atoi(pos);
  loopi(n) {
    s += strcspn(s, " \0");
    s += strspn(s, " ");
  }
  s[strcspn(s, " \0")] = 0;
  concat(s);
}

CMD(at, ARG_2STR);
CMDN(while, whilea, ARG_2STR);
CMDN(loop, loopa, ARG_2STR);
CMDN(if, ifthen, ARG_3STR); 
CMD(onrelease, ARG_DWN1);
CMD(exec, ARG_1STR);
CMD(concat, ARG_VARI);
CMD(concatword, ARG_VARI);
CMD(listlen, ARG_1EST);

static int add(int a, int b)   { return a+b; } CMDN(+, add, ARG_2EXP);
static int mul(int a, int b)   { return a*b; } CMDN(*, mul, ARG_2EXP);
static int sub(int a, int b)   { return a-b; } CMDN(-, sub, ARG_2EXP);
static int divi(int a, int b)  { return b ? a/b : 0; } CMDN(div, divi, ARG_2EXP);
static int mod(int a, int b)   { return b ? a%b : 0; } CMD(mod, ARG_2EXP);
static int equal(int a, int b) { return (int)(a==b); } CMDN(=, equal, ARG_2EXP);
static int lt(int a, int b)    { return (int)(a<b); }  CMDN(<, lt, ARG_2EXP);
static int gt(int a, int b)    { return (int)(a>b); }  CMDN(>, gt, ARG_2EXP);
static int rndn(int a)         { return a>0 ? rnd(a) : 0; }  CMDN(rnd, rndn, ARG_1EXP);
static int strcmpa(char *a, char *b) { return strcmp(a,b)==0; }  CMDN(strcmp, strcmpa, ARG_2EST);
} /* namespace cmd */
} /* namespace q */

