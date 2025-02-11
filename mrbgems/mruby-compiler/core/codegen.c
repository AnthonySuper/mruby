/*
** codegen.c - mruby code generator
**
** See Copyright Notice in mruby.h
*/

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/proc.h>
#include <mruby/numeric.h>
#include <mruby/string.h>
#include <mruby/debug.h>
#include "node.h"
#include <mruby/opcode.h>
#include <mruby/re.h>
#include <mruby/throw.h>

typedef mrb_ast_node node;
typedef struct mrb_parser_state parser_state;

enum looptype {
  LOOP_NORMAL,
  LOOP_BLOCK,
  LOOP_FOR,
  LOOP_BEGIN,
  LOOP_RESCUE,
};

struct loopinfo {
  enum looptype type;
  int pc1, pc2, pc3, acc;
  int ensure_level;
  struct loopinfo *prev;
};

typedef struct scope {
  mrb_state *mrb;
  mrb_pool *mpool;
  struct mrb_jmpbuf jmp;

  struct scope *prev;

  node *lv;

  int sp;
  int pc;
  int lastlabel;
  int ainfo:15;
  mrb_bool mscope:1;

  struct loopinfo *loop;
  int ensure_level;
  char const *filename;
  uint16_t lineno;

  mrb_code *iseq;
  uint16_t *lines;
  int icapa;

  mrb_irep *irep;
  size_t pcapa;
  size_t scapa;
  size_t rcapa;

  uint16_t nlocals;
  uint16_t nregs;
  int ai;

  int debug_start_pos;
  uint16_t filename_index;
  parser_state* parser;
} codegen_scope;

static codegen_scope* scope_new(mrb_state *mrb, codegen_scope *prev, node *lv);
static void scope_finish(codegen_scope *s);
static struct loopinfo *loop_push(codegen_scope *s, enum looptype t);
static void loop_break(codegen_scope *s, node *tree);
static void loop_pop(codegen_scope *s, int val);

static void gen_assignment(codegen_scope *s, node *tree, int sp, int val);
static void gen_vmassignment(codegen_scope *s, node *tree, int rhs, int val);

static void codegen(codegen_scope *s, node *tree, int val);
static void raise_error(codegen_scope *s, const char *msg);

static void
codegen_error(codegen_scope *s, const char *message)
{
  if (!s) return;
  while (s->prev) {
    codegen_scope *tmp = s->prev;
    mrb_pool_close(s->mpool);
    s = tmp;
  }
#ifndef MRB_DISABLE_STDIO
  if (s->filename && s->lineno) {
    fprintf(stderr, "codegen error:%s:%d: %s\n", s->filename, s->lineno, message);
  }
  else {
    fprintf(stderr, "codegen error: %s\n", message);
  }
#endif
  MRB_THROW(&s->jmp);
}

static void*
codegen_palloc(codegen_scope *s, size_t len)
{
  void *p = mrb_pool_alloc(s->mpool, len);

  if (!p) codegen_error(s, "pool memory allocation");
  return p;
}

static void*
codegen_malloc(codegen_scope *s, size_t len)
{
  void *p = mrb_malloc_simple(s->mrb, len);

  if (!p) codegen_error(s, "mrb_malloc");
  return p;
}

static void*
codegen_realloc(codegen_scope *s, void *p, size_t len)
{
  p = mrb_realloc_simple(s->mrb, p, len);

  if (!p && len > 0) codegen_error(s, "mrb_realloc");
  return p;
}

static int
new_label(codegen_scope *s)
{
  s->lastlabel = s->pc;
  return s->pc;
}

static inline int
genop(codegen_scope *s, mrb_code i)
{
  if (s->pc == s->icapa) {
    s->icapa *= 2;
    s->iseq = (mrb_code *)codegen_realloc(s, s->iseq, sizeof(mrb_code)*s->icapa);
    if (s->lines) {
      s->lines = (uint16_t*)codegen_realloc(s, s->lines, sizeof(short)*s->icapa);
      s->irep->lines = s->lines;
    }
  }
  s->iseq[s->pc] = i;
  if (s->lines) {
    s->lines[s->pc] = s->lineno;
  }
  return s->pc++;
}

#define NOVAL  0
#define VAL    1

static mrb_bool
no_optimize(codegen_scope *s)
{
  if (s && s->parser && s->parser->no_optimize)
    return TRUE;
  return FALSE;
}

static int
genop_peep(codegen_scope *s, mrb_code i, int val)
{
  /* peephole optimization */
  if (!no_optimize(s) && s->lastlabel != s->pc && s->pc > 0) {
    mrb_code i0 = s->iseq[s->pc-1];
    int c1 = GET_OPCODE(i);
    int c0 = GET_OPCODE(i0);

    switch (c1) {
    case OP_MOVE:
      if (GETARG_A(i) == GETARG_B(i)) {
        /* skip useless OP_MOVE */
        return 0;
      }
      if (val) break;
      switch (c0) {
      case OP_MOVE:
        if (GETARG_A(i) == GETARG_A(i0)) {
          /* skip overriden OP_MOVE */
          s->pc--;
          s->iseq[s->pc] = i;
        }
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i) == GETARG_B(i0)) {
          /* skip swapping OP_MOVE */
          return 0;
        }
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->pc--;
          return genop_peep(s, MKOP_AB(OP_MOVE, GETARG_A(i), GETARG_B(i0)), val);
        }
        break;
      case OP_LOADI:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_AsBx(OP_LOADI, GETARG_A(i), GETARG_sBx(i0));
          return 0;
        }
        break;
      case OP_ARRAY:
      case OP_HASH:
      case OP_RANGE:
      case OP_AREF:
      case OP_GETUPVAR:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_ABC(c0, GETARG_A(i), GETARG_B(i0), GETARG_C(i0));
          return 0;
        }
        break;
      case OP_LOADSYM:
      case OP_GETGLOBAL:
      case OP_GETIV:
      case OP_GETCV:
      case OP_GETCONST:
      case OP_GETSPECIAL:
      case OP_LOADL:
      case OP_STRING:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_ABx(c0, GETARG_A(i), GETARG_Bx(i0));
          return 0;
        }
        break;
      case OP_SCLASS:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_AB(c0, GETARG_A(i), GETARG_B(i0));
          return 0;
        }
        break;
      case OP_LOADNIL:
      case OP_LOADSELF:
      case OP_LOADT:
      case OP_LOADF:
      case OP_OCLASS:
        if (GETARG_B(i) == GETARG_A(i0) && GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_A(c0, GETARG_A(i));
          return 0;
        }
        break;
      default:
        break;
      }
      break;
    case OP_SETIV:
    case OP_SETCV:
    case OP_SETCONST:
    case OP_SETMCNST:
    case OP_SETGLOBAL:
      if (val) break;
      if (c0 == OP_MOVE) {
        if (GETARG_A(i) == GETARG_A(i0)) {
          s->iseq[s->pc-1] = MKOP_ABx(c1, GETARG_B(i0), GETARG_Bx(i));
          return 0;
        }
      }
      break;
    case OP_SETUPVAR:
      if (val) break;
      if (c0 == OP_MOVE) {
        if (GETARG_A(i) == GETARG_A(i0)) {
          s->iseq[s->pc-1] = MKOP_ABC(c1, GETARG_B(i0), GETARG_B(i), GETARG_C(i));
          return 0;
        }
      }
      break;
    case OP_EPOP:
      if (c0 == OP_EPOP) {
        s->iseq[s->pc-1] = MKOP_A(OP_EPOP, GETARG_A(i0)+GETARG_A(i));
        return 0;
      }
      break;
    case OP_POPERR:
      if (c0 == OP_POPERR) {
        s->iseq[s->pc-1] = MKOP_A(OP_POPERR, GETARG_A(i0)+GETARG_A(i));
        return 0;
      }
      break;
    case OP_RETURN:
      switch (c0) {
      case OP_RETURN:
        return 0;
      case OP_MOVE:
        if (GETARG_A(i0) >= s->nlocals) {
          s->iseq[s->pc-1] = MKOP_AB(OP_RETURN, GETARG_B(i0), OP_R_NORMAL);
          return 0;
        }
        break;
      case OP_SETIV:
      case OP_SETCV:
      case OP_SETCONST:
      case OP_SETMCNST:
      case OP_SETUPVAR:
      case OP_SETGLOBAL:
        s->pc--;
        genop_peep(s, i0, NOVAL);
        i0 = s->iseq[s->pc-1];
        return genop(s, MKOP_AB(OP_RETURN, GETARG_A(i0), OP_R_NORMAL));
#if 0
      case OP_SEND:
        if (GETARG_B(i) == OP_R_NORMAL && GETARG_A(i) == GETARG_A(i0)) {
          s->iseq[s->pc-1] = MKOP_ABC(OP_TAILCALL, GETARG_A(i0), GETARG_B(i0), GETARG_C(i0));
          return;
        }
        break;
#endif
      default:
        break;
      }
      break;
    case OP_ADD:
    case OP_SUB:
      if (c0 == OP_LOADI) {
        int c = GETARG_sBx(i0);

        if (c1 == OP_SUB) c = -c;
        if (c > 127 || c < -127) break;
        if (0 <= c)
          s->iseq[s->pc-1] = MKOP_ABC(OP_ADDI, GETARG_A(i), GETARG_B(i), c);
        else
          s->iseq[s->pc-1] = MKOP_ABC(OP_SUBI, GETARG_A(i), GETARG_B(i), -c);
        return 0;
      }
    case OP_STRCAT:
      if (c0 == OP_STRING) {
        mrb_value v = s->irep->pool[GETARG_Bx(i0)];

        if (mrb_string_p(v) && RSTRING_LEN(v) == 0) {
          s->pc--;
          return 0;
        }
      }
      if (c0 == OP_LOADNIL) {
        if (GETARG_B(i) == GETARG_A(i0)) {
          s->pc--;
          return 0;
        }
      }
      break;
    case OP_JMPIF:
    case OP_JMPNOT:
      if (c0 == OP_MOVE && GETARG_A(i) == GETARG_A(i0)) {
        s->iseq[s->pc-1] = MKOP_AsBx(c1, GETARG_B(i0), GETARG_sBx(i));
        return s->pc-1;
      }
      break;
    default:
      break;
    }
  }
  return genop(s, i);
}

static void
scope_error(codegen_scope *s)
{
  exit(EXIT_FAILURE);
}

static inline void
dispatch(codegen_scope *s, int pc)
{
  int diff = s->pc - pc;
  mrb_code i = s->iseq[pc];
  int c = GET_OPCODE(i);

  s->lastlabel = s->pc;
  switch (c) {
  case OP_JMP:
  case OP_JMPIF:
  case OP_JMPNOT:
  case OP_ONERR:
    break;
  default:
#ifndef MRB_DISABLE_STDIO
    fprintf(stderr, "bug: dispatch on non JMP op\n");
#endif
    scope_error(s);
    break;
  }
  s->iseq[pc] = MKOP_AsBx(c, GETARG_A(i), diff);
}

static void
dispatch_linked(codegen_scope *s, int pc)
{
  mrb_code i;
  int pos;

  if (!pc) return;
  for (;;) {
    i = s->iseq[pc];
    pos = GETARG_sBx(i);
    dispatch(s, pc);
    if (!pos) break;
    pc = pos;
  }
}

#define nregs_update do {if (s->sp > s->nregs) s->nregs = s->sp;} while (0)
static void
push_(codegen_scope *s)
{
  if (s->sp > 511) {
    codegen_error(s, "too complex expression");
  }
  s->sp++;
  nregs_update;
}

static void
push_n_(codegen_scope *s, size_t n)
{
  if (s->sp+n > 511) {
    codegen_error(s, "too complex expression");
  }
  s->sp+=n;
  nregs_update;
}

#define push() push_(s)
#define push_n(n) push_n_(s,n)
#define pop_(s) ((s)->sp--)
#define pop() pop_(s)
#define pop_n(n) (s->sp-=(n))
#define cursp() (s->sp)

static inline int
new_lit(codegen_scope *s, mrb_value val)
{
  size_t i;
  mrb_value *pv;

  switch (mrb_type(val)) {
  case MRB_TT_STRING:
    for (i=0; i<s->irep->plen; i++) {
      mrb_int len;
      pv = &s->irep->pool[i];

      if (mrb_type(*pv) != MRB_TT_STRING) continue;
      if ((len = RSTRING_LEN(*pv)) != RSTRING_LEN(val)) continue;
      if (memcmp(RSTRING_PTR(*pv), RSTRING_PTR(val), len) == 0)
        return i;
    }
    break;
  case MRB_TT_FLOAT:
    for (i=0; i<s->irep->plen; i++) {
      pv = &s->irep->pool[i];
      if (mrb_type(*pv) != MRB_TT_FLOAT) continue;
      if (mrb_float(*pv) == mrb_float(val)) return i;
    }
    break;
  case MRB_TT_FIXNUM:
    for (i=0; i<s->irep->plen; i++) {
      pv = &s->irep->pool[i];
      if (!mrb_fixnum_p(*pv)) continue;
      if (mrb_fixnum(*pv) == mrb_fixnum(val)) return i;
    }
    break;
  default:
    /* should not happen */
    return 0;
  }

  if (s->irep->plen == s->pcapa) {
    s->pcapa *= 2;
    s->irep->pool = (mrb_value *)codegen_realloc(s, s->irep->pool, sizeof(mrb_value)*s->pcapa);
  }

  pv = &s->irep->pool[s->irep->plen];
  i = s->irep->plen++;

  switch (mrb_type(val)) {
  case MRB_TT_STRING:
    *pv = mrb_str_pool(s->mrb, val);
    break;

  case MRB_TT_FLOAT:
#ifdef MRB_WORD_BOXING
    *pv = mrb_float_pool(s->mrb, mrb_float(val));
    break;
#endif
  case MRB_TT_FIXNUM:
    *pv = val;
    break;

  default:
    /* should not happen */
    break;
  }
  return i;
}

static inline int
new_msym(codegen_scope *s, mrb_sym sym)
{
  size_t i, len;

  mrb_assert(s->irep);

  len = s->irep->slen;
  if (len > 256) len = 256;
  for (i=0; i<len; i++) {
    if (s->irep->syms[i] == sym) return i;
    if (s->irep->syms[i] == 0) break;
  }
  if (i == 256) {
    codegen_error(s, "too many symbols (max 256)");
  }
  s->irep->syms[i] = sym;
  if (i == s->irep->slen) s->irep->slen++;
  return i;
}

static inline int
new_sym(codegen_scope *s, mrb_sym sym)
{
  size_t i;

  for (i=0; i<s->irep->slen; i++) {
    if (s->irep->syms[i] == sym) return i;
  }
  if (s->irep->slen > 125 && s->irep->slen < 256) {
    s->irep->syms = (mrb_sym *)codegen_realloc(s, s->irep->syms, sizeof(mrb_sym)*65536);
    for (i = 0; i < 256 - s->irep->slen; i++) {
      static const mrb_sym mrb_sym_zero = { 0 };
      s->irep->syms[i + s->irep->slen] = mrb_sym_zero;
    }
    s->irep->slen = 256;
  }
  s->irep->syms[s->irep->slen] = sym;
  return s->irep->slen++;
}

static int
node_len(node *tree)
{
  int n = 0;

  while (tree) {
    n++;
    tree = tree->cdr;
  }
  return n;
}

#define sym(x) ((mrb_sym)(intptr_t)(x))
#define lv_name(lv) sym((lv)->car)
static int
lv_idx(codegen_scope *s, mrb_sym id)
{
  node *lv = s->lv;
  int n = 1;

  while (lv) {
    if (lv_name(lv) == id) return n;
    n++;
    lv = lv->cdr;
  }
  return 0;
}

static void
for_body(codegen_scope *s, node *tree)
{
  codegen_scope *prev = s;
  int idx;
  struct loopinfo *lp;
  node *n2;
  mrb_code c;

  /* generate receiver */
  codegen(s, tree->cdr->car, VAL);
  /* generate loop-block */
  s = scope_new(s->mrb, s, NULL);
  if (s == NULL) {
    raise_error(prev, "unexpected scope");
  }

  push();                       /* push for a block parameter */

  lp = loop_push(s, LOOP_FOR);
  lp->pc1 = new_label(s);

  /* generate loop variable */
  n2 = tree->car;
  genop(s, MKOP_Ax(OP_ENTER, 0x40000));
  if (n2->car && !n2->car->cdr && !n2->cdr) {
    gen_assignment(s, n2->car->car, 1, NOVAL);
  }
  else {
    gen_vmassignment(s, n2, 1, VAL);
  }
  codegen(s, tree->cdr->cdr->car, VAL);
  pop();
  if (s->pc > 0) {
    c = s->iseq[s->pc-1];
    if (GET_OPCODE(c) != OP_RETURN || GETARG_B(c) != OP_R_NORMAL || s->pc == s->lastlabel)
      genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
  }
  loop_pop(s, NOVAL);
  scope_finish(s);
  s = prev;
  genop(s, MKOP_Abc(OP_LAMBDA, cursp(), s->irep->rlen-1, OP_L_BLOCK));
  pop();
  idx = new_msym(s, mrb_intern_lit(s->mrb, "each"));
  genop(s, MKOP_ABC(OP_SENDB, cursp(), idx, 0));
}

static int
lambda_body(codegen_scope *s, node *tree, int blk)
{
  mrb_code c;
  codegen_scope *parent = s;
  s = scope_new(s->mrb, s, tree->car);
  if (s == NULL) {
    raise_error(parent, "unexpected scope");
  }

  s->mscope = !blk;

  if (blk) {
    struct loopinfo *lp = loop_push(s, LOOP_BLOCK);
    lp->pc1 = new_label(s);
  }
  tree = tree->cdr;
  if (tree->car) {
    mrb_aspec a;
    int ma, oa, ra, pa, ka, kd, ba;
    int pos, i;
    node *n, *opt;

    ma = node_len(tree->car->car);
    n = tree->car->car;
    while (n) {
      n = n->cdr;
    }
    oa = node_len(tree->car->cdr->car);
    ra = tree->car->cdr->cdr->car ? 1 : 0;
    pa = node_len(tree->car->cdr->cdr->cdr->car);
    ka = kd = 0;
    ba = tree->car->cdr->cdr->cdr->cdr ? 1 : 0;

    a = ((mrb_aspec)(ma & 0x1f) << 18)
      | ((mrb_aspec)(oa & 0x1f) << 13)
      | ((ra & 1) << 12)
      | ((pa & 0x1f) << 7)
      | ((ka & 0x1f) << 2)
      | ((kd & 1)<< 1)
      | (ba & 1);
    s->ainfo = (((ma+oa) & 0x3f) << 6) /* (12bits = 6:1:5) */
      | ((ra & 1) << 5)
      | (pa & 0x1f);
    genop(s, MKOP_Ax(OP_ENTER, a));
    pos = new_label(s);
    for (i=0; i<oa; i++) {
      new_label(s);
      genop(s, MKOP_sBx(OP_JMP, 0));
    }
    if (oa > 0) {
      genop(s, MKOP_sBx(OP_JMP, 0));
    }
    opt = tree->car->cdr->car;
    i = 0;
    while (opt) {
      int idx;

      dispatch(s, pos+i);
      codegen(s, opt->car->cdr, VAL);
      idx = lv_idx(s, (mrb_sym)(intptr_t)opt->car->car);
      pop();
      genop_peep(s, MKOP_AB(OP_MOVE, idx, cursp()), NOVAL);
      i++;
      opt = opt->cdr;
    }
    if (oa > 0) {
      dispatch(s, pos+i);
    }
  }
  codegen(s, tree->cdr->car, VAL);
  pop();
  if (s->pc > 0) {
    c = s->iseq[s->pc-1];
    if (GET_OPCODE(c) != OP_RETURN || GETARG_B(c) != OP_R_NORMAL || s->pc == s->lastlabel) {
      if (s->nregs == 0) {
        genop(s, MKOP_A(OP_LOADNIL, 0));
        genop(s, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
      }
      else {
        genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
      }
    }
  }
  if (blk) {
    loop_pop(s, NOVAL);
  }
  scope_finish(s);
  return parent->irep->rlen - 1;
}

static int
scope_body(codegen_scope *s, node *tree, int val)
{
  codegen_scope *scope = scope_new(s->mrb, s, tree->car);
  if (scope == NULL) {
    raise_error(s, "unexpected scope");
  }

  codegen(scope, tree->cdr, VAL);
  if (!s->iseq) {
    genop(scope, MKOP_A(OP_STOP, 0));
  }
  else if (!val) {
    genop(scope, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
  }
  else {
    if (scope->nregs == 0) {
      genop(scope, MKOP_A(OP_LOADNIL, 0));
      genop(scope, MKOP_AB(OP_RETURN, 0, OP_R_NORMAL));
    }
    else {
      genop_peep(scope, MKOP_AB(OP_RETURN, scope->sp-1, OP_R_NORMAL), NOVAL);
    }
  }
  scope_finish(scope);
  if (!s->irep) {
    /* should not happen */
    return 0;
  }
  return s->irep->rlen - 1;
}

static mrb_bool
nosplat(node *t)
{
  while (t) {
    if ((intptr_t)t->car->car == NODE_SPLAT) return FALSE;
    t = t->cdr;
  }
  return TRUE;
}

static mrb_sym
attrsym(codegen_scope *s, mrb_sym a)
{
  const char *name;
  mrb_int len;
  char *name2;

  name = mrb_sym2name_len(s->mrb, a, &len);
  name2 = (char *)codegen_palloc(s,
                                 (size_t)len
                                 + 1 /* '=' */
                                 + 1 /* '\0' */
                                 );
  mrb_assert_int_fit(mrb_int, len, size_t, SIZE_MAX);
  memcpy(name2, name, (size_t)len);
  name2[len] = '=';
  name2[len+1] = '\0';

  return mrb_intern(s->mrb, name2, len+1);
}

static int
gen_values(codegen_scope *s, node *t, int val)
{
  int n = 0;
  int is_splat;

  while (t) {
    is_splat = (intptr_t)t->car->car == NODE_SPLAT; /* splat mode */
    if (n >= 127 || is_splat) {
      if (val) {
        if (is_splat && n == 0 && (intptr_t)t->car->cdr->car == NODE_ARRAY) {
          codegen(s, t->car->cdr, VAL);
          pop();
        }
        else {
          pop_n(n);
          genop(s, MKOP_ABC(OP_ARRAY, cursp(), cursp(), n));
          push();
          codegen(s, t->car, VAL);
          pop(); pop();
          if (is_splat) {
            genop(s, MKOP_AB(OP_ARYCAT, cursp(), cursp()+1));
          }
          else {
            genop(s, MKOP_AB(OP_ARYPUSH, cursp(), cursp()+1));
          }
        }
        t = t->cdr;
        while (t) {
          push();
          codegen(s, t->car, VAL);
          pop(); pop();
          if ((intptr_t)t->car->car == NODE_SPLAT) {
            genop(s, MKOP_AB(OP_ARYCAT, cursp(), cursp()+1));
          }
          else {
            genop(s, MKOP_AB(OP_ARYPUSH, cursp(), cursp()+1));
          }
          t = t->cdr;
        }
      }
      else {
        codegen(s, t->car->cdr, NOVAL);
        t = t->cdr;
        while (t) {
          codegen(s, t->car, NOVAL);
          t = t->cdr;
        }
      }
      return -1;
    }
    /* normal (no splat) mode */
    codegen(s, t->car, val);
    n++;
    t = t->cdr;
  }
  return n;
}

#define CALL_MAXARGS 127

static void
gen_call(codegen_scope *s, node *tree, mrb_sym name, int sp, int val, int safe)
{
  mrb_sym sym = name ? name : sym(tree->cdr->car);
  int idx, skip;
  int n = 0, noop = 0, sendv = 0, blk = 0;

  codegen(s, tree->car, VAL); /* receiver */
  if (safe) {
    int recv = cursp()-1;
    genop(s, MKOP_A(OP_LOADNIL, cursp()));
    push();
    genop(s, MKOP_AB(OP_MOVE, cursp(), recv));
    pop();
    idx = new_msym(s, mrb_intern_lit(s->mrb, "=="));
    genop(s, MKOP_ABC(OP_EQ, cursp(), idx, 1));
    skip = genop(s, MKOP_AsBx(OP_JMPIF, cursp(), 0));
  }
  idx = new_msym(s, sym);
  tree = tree->cdr->cdr->car;
  if (tree) {
    n = gen_values(s, tree->car, VAL);
    if (n < 0) {
      n = noop = sendv = 1;
      push();
    }
  }
  if (sp) {
    if (sendv) {
      pop();
      genop(s, MKOP_AB(OP_ARYPUSH, cursp(), sp));
      push();
    }
    else {
      genop(s, MKOP_AB(OP_MOVE, cursp(), sp));
      push();
      n++;
    }
  }
  if (tree && tree->cdr) {
    noop = 1;
    codegen(s, tree->cdr, VAL);
    pop();
  }
  else {
    blk = cursp();
  }
  push();pop();
  pop_n(n+1);
  {
    mrb_int symlen;
    const char *symname = mrb_sym2name_len(s->mrb, sym, &symlen);

    if (!noop && symlen == 1 && symname[0] == '+')  {
      genop_peep(s, MKOP_ABC(OP_ADD, cursp(), idx, n), val);
    }
    else if (!noop && symlen == 1 && symname[0] == '-')  {
      genop_peep(s, MKOP_ABC(OP_SUB, cursp(), idx, n), val);
    }
    else if (!noop && symlen == 1 && symname[0] == '*')  {
      genop(s, MKOP_ABC(OP_MUL, cursp(), idx, n));
    }
    else if (!noop && symlen == 1 && symname[0] == '/')  {
      genop(s, MKOP_ABC(OP_DIV, cursp(), idx, n));
    }
    else if (!noop && symlen == 1 && symname[0] == '<')  {
      genop(s, MKOP_ABC(OP_LT, cursp(), idx, n));
    }
    else if (!noop && symlen == 2 && symname[0] == '<' && symname[1] == '=')  {
      genop(s, MKOP_ABC(OP_LE, cursp(), idx, n));
    }
    else if (!noop && symlen == 1 && symname[0] == '>')  {
      genop(s, MKOP_ABC(OP_GT, cursp(), idx, n));
    }
    else if (!noop && symlen == 2 && symname[0] == '>' && symname[1] == '=')  {
      genop(s, MKOP_ABC(OP_GE, cursp(), idx, n));
    }
    else if (!noop && symlen == 2 && symname[0] == '=' && symname[1] == '=')  {
      genop(s, MKOP_ABC(OP_EQ, cursp(), idx, n));
    }
    else {
      if (sendv) n = CALL_MAXARGS;
      if (blk > 0) {                   /* no block */
        genop(s, MKOP_ABC(OP_SEND, cursp(), idx, n));
      }
      else {
        genop(s, MKOP_ABC(OP_SENDB, cursp(), idx, n));
      }
    }
  }
  if (safe) {
    dispatch(s, skip);
  }
  if (val) {
    push();
  }
}

static void
gen_assignment(codegen_scope *s, node *tree, int sp, int val)
{
  int idx;
  int type = (intptr_t)tree->car;

  tree = tree->cdr;
  switch ((intptr_t)type) {
  case NODE_GVAR:
    idx = new_sym(s, sym(tree));
    genop_peep(s, MKOP_ABx(OP_SETGLOBAL, sp, idx), val);
    break;
  case NODE_LVAR:
    idx = lv_idx(s, sym(tree));
    if (idx > 0) {
      if (idx != sp) {
        genop_peep(s, MKOP_AB(OP_MOVE, idx, sp), val);
      }
      break;
    }
    else {                      /* upvar */
      int lv = 0;
      codegen_scope *up = s->prev;

      while (up) {
        idx = lv_idx(up, sym(tree));
        if (idx > 0) {
          genop_peep(s, MKOP_ABC(OP_SETUPVAR, sp, idx, lv), val);
          break;
        }
        lv++;
        up = up->prev;
      }
    }
    break;
  case NODE_IVAR:
    idx = new_sym(s, sym(tree));
    genop_peep(s, MKOP_ABx(OP_SETIV, sp, idx), val);
    break;
  case NODE_CVAR:
    idx = new_sym(s, sym(tree));
    genop_peep(s, MKOP_ABx(OP_SETCV, sp, idx), val);
    break;
  case NODE_CONST:
    idx = new_sym(s, sym(tree));
    genop_peep(s, MKOP_ABx(OP_SETCONST, sp, idx), val);
    break;
  case NODE_COLON2:
    idx = new_sym(s, sym(tree->cdr));
    genop_peep(s, MKOP_AB(OP_MOVE, cursp(), sp), NOVAL);
    push();
    codegen(s, tree->car, VAL);
    pop_n(2);
    genop_peep(s, MKOP_ABx(OP_SETMCNST, cursp(), idx), val);
    break;

  case NODE_CALL:
    push();
    gen_call(s, tree, attrsym(s, sym(tree->cdr->car)), sp, NOVAL, 0);
    pop();
    if (val) {
      genop_peep(s, MKOP_AB(OP_MOVE, cursp(), sp), val);
    }
    break;

  case NODE_MASGN:
    gen_vmassignment(s, tree->car, sp, val);
    break;

  /* splat without assignment */
  case NODE_NIL:
    break;

  default:
#ifndef MRB_DISABLE_STDIO
    printf("unknown lhs %d\n", type);
#endif
    break;
  }
  if (val) push();
}

static void
gen_vmassignment(codegen_scope *s, node *tree, int rhs, int val)
{
  int n = 0, post = 0;
  node *t, *p;

  if (tree->car) {              /* pre */
    t = tree->car;
    n = 0;
    while (t) {
      genop(s, MKOP_ABC(OP_AREF, cursp(), rhs, n));
      gen_assignment(s, t->car, cursp(), NOVAL);
      n++;
      t = t->cdr;
    }
  }
  t = tree->cdr;
  if (t) {
    if (t->cdr) {               /* post count */
      p = t->cdr->car;
      while (p) {
        post++;
        p = p->cdr;
      }
    }
    if (val) {
      genop(s, MKOP_AB(OP_MOVE, cursp(), rhs));
    }
    else {
      pop();
    }
    push_n(post);
    pop_n(post);
    genop(s, MKOP_ABC(OP_APOST, cursp(), n, post));
    n = 1;
    if (t->car) {               /* rest */
      gen_assignment(s, t->car, cursp(), NOVAL);
    }
    if (t->cdr && t->cdr->car) {
      t = t->cdr->car;
      while (t) {
        gen_assignment(s, t->car, cursp()+n, NOVAL);
        t = t->cdr;
        n++;
      }
    }
    push();
  }
}

static void
gen_send_intern(codegen_scope *s)
{
  pop();
  genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "intern")), 0));
  push();
}
static void
gen_literal_array(codegen_scope *s, node *tree, mrb_bool sym, int val)
{
  if (val) {
    int i = 0, j = 0;

    while (tree) {
      switch ((intptr_t)tree->car->car) {
      case NODE_STR:
        if ((tree->cdr == NULL) && ((intptr_t)tree->car->cdr->cdr == 0))
          break;
        /* fall through */
      case NODE_BEGIN:
        codegen(s, tree->car, VAL);
        ++j;
        break;

      case NODE_LITERAL_DELIM:
        if (j > 0) {
          j = 0;
          ++i;
          if (sym)
            gen_send_intern(s);
        }
        break;
      }
      if (j >= 2) {
        pop(); pop();
        genop_peep(s, MKOP_AB(OP_STRCAT, cursp(), cursp()+1), VAL);
        push();
        j = 1;
      }
      tree = tree->cdr;
    }
    if (j > 0) {
      ++i;
      if (sym)
        gen_send_intern(s);
    }
    pop_n(i);
    genop(s, MKOP_ABC(OP_ARRAY, cursp(), cursp(), i));
    push();
  }
  else {
    while (tree) {
      switch ((intptr_t)tree->car->car) {
      case NODE_BEGIN: case NODE_BLOCK:
        codegen(s, tree->car, NOVAL);
      }
      tree = tree->cdr;
    }
  }
}

static void
raise_error(codegen_scope *s, const char *msg)
{
  int idx = new_lit(s, mrb_str_new_cstr(s->mrb, msg));

  genop(s, MKOP_ABx(OP_ERR, 1, idx));
}

static double
readint_float(codegen_scope *s, const char *p, int base)
{
  const char *e = p + strlen(p);
  double f = 0;
  int n;

  if (*p == '+') p++;
  while (p < e) {
    char c = *p;
    c = tolower((unsigned char)c);
    for (n=0; n<base; n++) {
      if (mrb_digitmap[n] == c) {
        f *= base;
        f += n;
        break;
      }
    }
    if (n == base) {
      codegen_error(s, "malformed readint input");
    }
    p++;
  }
  return f;
}

static mrb_int
readint_mrb_int(codegen_scope *s, const char *p, int base, mrb_bool neg, mrb_bool *overflow)
{
  const char *e = p + strlen(p);
  mrb_int result = 0;
  int n;

  mrb_assert(base >= 2 && base <= 36);
  if (*p == '+') p++;
  while (p < e) {
    char c = *p;
    c = tolower((unsigned char)c);
    for (n=0; n<base; n++) {
      if (mrb_digitmap[n] == c) {
        break;
      }
    }
    if (n == base) {
      codegen_error(s, "malformed readint input");
    }

    if (neg) {
      if ((MRB_INT_MIN + n)/base > result) {
        *overflow = TRUE;
        return 0;
      }
      result *= base;
      result -= n;
    }
    else {
      if ((MRB_INT_MAX - n)/base < result) {
        *overflow = TRUE;
        return 0;
      }
      result *= base;
      result += n;
    }
    p++;
  }
  *overflow = FALSE;
  return result;
}

static void
codegen(codegen_scope *s, node *tree, int val)
{
  int nt;

  if (!tree) {
    if (val) {
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      push();
    }
    return;
  }

  if (s->irep && s->filename_index != tree->filename_index) {
    s->irep->filename = mrb_parser_get_filename(s->parser, s->filename_index);
    mrb_debug_info_append_file(s->mrb, s->irep, s->debug_start_pos, s->pc);
    s->debug_start_pos = s->pc;
    s->filename_index = tree->filename_index;
    s->filename = mrb_parser_get_filename(s->parser, tree->filename_index);
  }

  nt = (intptr_t)tree->car;
  s->lineno = tree->lineno;
  tree = tree->cdr;
  switch (nt) {
  case NODE_BEGIN:
    if (val && !tree) {
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      push();
    }
    while (tree) {
      codegen(s, tree->car, tree->cdr ? NOVAL : val);
      tree = tree->cdr;
    }
    break;

  case NODE_RESCUE:
    {
      int onerr, noexc, exend, pos1, pos2, tmp;
      struct loopinfo *lp;

      if (tree->car == NULL) return;
      onerr = genop(s, MKOP_Bx(OP_ONERR, 0));
      lp = loop_push(s, LOOP_BEGIN);
      lp->pc1 = onerr;
      codegen(s, tree->car, VAL);
      pop();
      lp->type = LOOP_RESCUE;
      noexc = genop(s, MKOP_Bx(OP_JMP, 0));
      dispatch(s, onerr);
      tree = tree->cdr;
      exend = 0;
      pos1 = 0;
      if (tree->car) {
        node *n2 = tree->car;
        int exc = cursp();

        genop(s, MKOP_A(OP_RESCUE, exc));
        push();
        while (n2) {
          node *n3 = n2->car;
          node *n4 = n3->car;

          if (pos1) dispatch(s, pos1);
          pos2 = 0;
          do {
            if (n4) {
              codegen(s, n4->car, VAL);
            }
            else {
              genop(s, MKOP_ABx(OP_GETCONST, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "StandardError"))));
              push();
            }
            genop(s, MKOP_AB(OP_MOVE, cursp(), exc));
            pop();
            if (n4 && n4->car && (intptr_t)n4->car->car == NODE_SPLAT) {
              genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "__case_eqq")), 1));
            }
            else {
              genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "===")), 1));
            }
            tmp = genop(s, MKOP_AsBx(OP_JMPIF, cursp(), pos2));
            pos2 = tmp;
            if (n4) {
              n4 = n4->cdr;
            }
          } while (n4);
          pos1 = genop(s, MKOP_sBx(OP_JMP, 0));
          dispatch_linked(s, pos2);

          pop();
          if (n3->cdr->car) {
            gen_assignment(s, n3->cdr->car, exc, NOVAL);
          }
          if (n3->cdr->cdr->car) {
            codegen(s, n3->cdr->cdr->car, val);
            if (val) pop();
          }
          tmp = genop(s, MKOP_sBx(OP_JMP, exend));
          exend = tmp;
          n2 = n2->cdr;
          push();
        }
        if (pos1) {
          dispatch(s, pos1);
          genop(s, MKOP_A(OP_RAISE, exc));
        }
      }
      pop();
      tree = tree->cdr;
      dispatch(s, noexc);
      genop(s, MKOP_A(OP_POPERR, 1));
      if (tree->car) {
        codegen(s, tree->car, val);
      }
      else if (val) {
        push();
      }
      dispatch_linked(s, exend);
      loop_pop(s, NOVAL);
    }
    break;

  case NODE_ENSURE:
    {
      int idx;
      int epush = s->pc;

      genop(s, MKOP_Bx(OP_EPUSH, 0));
      s->ensure_level++;
      codegen(s, tree->car, val);
      idx = scope_body(s, tree->cdr, NOVAL);
      s->iseq[epush] = MKOP_Bx(OP_EPUSH, idx);
      s->ensure_level--;
      genop_peep(s, MKOP_A(OP_EPOP, 1), NOVAL);
    }
    break;

  case NODE_LAMBDA:
    {
      int idx = lambda_body(s, tree, 1);

      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_LAMBDA));
      push();
    }
    break;

  case NODE_BLOCK:
    {
      int idx = lambda_body(s, tree, 1);

      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_BLOCK));
      push();
    }
    break;

  case NODE_IF:
    {
      int pos1, pos2;
      node *e = tree->cdr->cdr->car;

      switch ((intptr_t)tree->car->car) {
      case NODE_TRUE:
      case NODE_INT:
      case NODE_STR:
        codegen(s, tree->cdr->car, val);
        return;
      case NODE_FALSE:
      case NODE_NIL:
        codegen(s, e, val);
        return;
      }
      codegen(s, tree->car, VAL);
      pop();
      pos1 = genop_peep(s, MKOP_AsBx(OP_JMPNOT, cursp(), 0), NOVAL);

      codegen(s, tree->cdr->car, val);
      if (e) {
        if (val) pop();
        pos2 = genop(s, MKOP_sBx(OP_JMP, 0));
        dispatch(s, pos1);
        codegen(s, e, val);
        dispatch(s, pos2);
      }
      else {
        if (val) {
          pop();
          pos2 = genop(s, MKOP_sBx(OP_JMP, 0));
          dispatch(s, pos1);
          genop(s, MKOP_A(OP_LOADNIL, cursp()));
          dispatch(s, pos2);
          push();
        }
        else {
          dispatch(s, pos1);
        }
      }
    }
    break;

  case NODE_AND:
    {
      int pos;

      codegen(s, tree->car, VAL);
      pop();
      pos = genop(s, MKOP_AsBx(OP_JMPNOT, cursp(), 0));
      codegen(s, tree->cdr, val);
      dispatch(s, pos);
    }
    break;

  case NODE_OR:
    {
      int pos;

      codegen(s, tree->car, VAL);
      pop();
      pos = genop(s, MKOP_AsBx(OP_JMPIF, cursp(), 0));
      codegen(s, tree->cdr, val);
      dispatch(s, pos);
    }
    break;

  case NODE_WHILE:
    {
      struct loopinfo *lp = loop_push(s, LOOP_NORMAL);

      lp->pc1 = genop(s, MKOP_sBx(OP_JMP, 0));
      lp->pc2 = new_label(s);
      codegen(s, tree->cdr, NOVAL);
      dispatch(s, lp->pc1);
      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_AsBx(OP_JMPIF, cursp(), lp->pc2 - s->pc));

      loop_pop(s, val);
    }
    break;

  case NODE_UNTIL:
    {
      struct loopinfo *lp = loop_push(s, LOOP_NORMAL);

      lp->pc1 = genop(s, MKOP_sBx(OP_JMP, 0));
      lp->pc2 = new_label(s);
      codegen(s, tree->cdr, NOVAL);
      dispatch(s, lp->pc1);
      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_AsBx(OP_JMPNOT, cursp(), lp->pc2 - s->pc));

      loop_pop(s, val);
    }
    break;

  case NODE_FOR:
    for_body(s, tree);
    if (val) push();
    break;

  case NODE_CASE:
    {
      int head = 0;
      int pos1, pos2, pos3, tmp;
      node *n;

      pos3 = 0;
      if (tree->car) {
        head = cursp();
        codegen(s, tree->car, VAL);
      }
      tree = tree->cdr;
      while (tree) {
        n = tree->car->car;
        pos1 = pos2 = 0;
        while (n) {
          codegen(s, n->car, VAL);
          if (head) {
            genop(s, MKOP_AB(OP_MOVE, cursp(), head));
            pop();
            if ((intptr_t)n->car->car == NODE_SPLAT) {
              genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "__case_eqq")), 1));
            }
            else {
              genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "===")), 1));
            }
          }
          else {
            pop();
          }
          tmp = genop(s, MKOP_AsBx(OP_JMPIF, cursp(), pos2));
          pos2 = tmp;
          n = n->cdr;
        }
        if (tree->car->car) {
          pos1 = genop(s, MKOP_sBx(OP_JMP, 0));
          dispatch_linked(s, pos2);
        }
        codegen(s, tree->car->cdr, val);
        if (val) pop();
        tmp = genop(s, MKOP_sBx(OP_JMP, pos3));
        pos3 = tmp;
        if (pos1) dispatch(s, pos1);
        tree = tree->cdr;
      }
      if (val) {
        int pos = cursp();
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        if (pos3) dispatch_linked(s, pos3);
        if (head) pop();
        genop(s, MKOP_AB(OP_MOVE, cursp(), pos));
        push();
      }
      else {
        if (pos3) {
          dispatch_linked(s, pos3);
        }
        if (head) {
          pop();
        }
      }
    }
    break;

  case NODE_SCOPE:
    scope_body(s, tree, NOVAL);
    break;

  case NODE_FCALL:
  case NODE_CALL:
    gen_call(s, tree, 0, 0, val, 0);
    break;
  case NODE_SCALL:
    gen_call(s, tree, 0, 0, val, 1);
    break;

  case NODE_DOT2:
    codegen(s, tree->car, val);
    codegen(s, tree->cdr, val);
    if (val) {
      pop(); pop();
      genop(s, MKOP_ABC(OP_RANGE, cursp(), cursp(), FALSE));
      push();
    }
    break;

  case NODE_DOT3:
    codegen(s, tree->car, val);
    codegen(s, tree->cdr, val);
    if (val) {
      pop(); pop();
      genop(s, MKOP_ABC(OP_RANGE, cursp(), cursp(), TRUE));
      push();
    }
    break;

  case NODE_COLON2:
    {
      int sym = new_sym(s, sym(tree->cdr));

      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      if (val) push();
    }
    break;

  case NODE_COLON3:
    {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_A(OP_OCLASS, cursp()));
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      if (val) push();
    }
    break;

  case NODE_ARRAY:
    {
      int n;

      n = gen_values(s, tree, val);
      if (n >= 0) {
        if (val) {
          pop_n(n);
          genop(s, MKOP_ABC(OP_ARRAY, cursp(), cursp(), n));
          push();
        }
      }
      else if (val) {
        push();
      }
    }
    break;

  case NODE_HASH:
    {
      int len = 0;
      mrb_bool update = FALSE;

      while (tree) {
        codegen(s, tree->car->car, val);
        codegen(s, tree->car->cdr, val);
        len++;
        tree = tree->cdr;
        if (val && len == 126) {
          pop_n(len*2);
          genop(s, MKOP_ABC(OP_HASH, cursp(), cursp(), len));
          if (update) {
            pop();
            genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "__update")), 1));
          }
          push();
          update = TRUE;
          len = 0;
        }
      }
      if (val) {
        pop_n(len*2);
        genop(s, MKOP_ABC(OP_HASH, cursp(), cursp(), len));
        if (update) {
          pop();
          genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "__update")), 1));
        }
        push();
      }
    }
    break;

  case NODE_SPLAT:
    codegen(s, tree, VAL);
    break;

  case NODE_ASGN:
    codegen(s, tree->cdr, VAL);
    pop();
    gen_assignment(s, tree->car, cursp(), val);
    break;

  case NODE_MASGN:
    {
      int len = 0, n = 0, post = 0;
      node *t = tree->cdr, *p;
      int rhs = cursp();

      if ((intptr_t)t->car == NODE_ARRAY && nosplat(t->cdr)) {
        /* fixed rhs */
        t = t->cdr;
        while (t) {
          codegen(s, t->car, VAL);
          len++;
          t = t->cdr;
        }
        tree = tree->car;
        if (tree->car) {                /* pre */
          t = tree->car;
          n = 0;
          while (t) {
            gen_assignment(s, t->car, rhs+n, NOVAL);
            n++;
            t = t->cdr;
          }
        }
        t = tree->cdr;
        if (t) {
          if (t->cdr) {         /* post count */
            p = t->cdr->car;
            while (p) {
              post++;
              p = p->cdr;
            }
          }
          if (t->car) {         /* rest (len - pre - post) */
            int rn;

            if (len < post + n) {
              rn = 0;
            }
            else {
              rn = len - post - n;
            }
            genop(s, MKOP_ABC(OP_ARRAY, cursp(), rhs+n, rn));
            gen_assignment(s, t->car, cursp(), NOVAL);
            n += rn;
          }
          if (t->cdr && t->cdr->car) {
            t = t->cdr->car;
            while (n<len) {
              gen_assignment(s, t->car, rhs+n, NOVAL);
              t = t->cdr;
              n++;
            }
          }
        }
        pop_n(len);
        if (val) {
          genop(s, MKOP_ABC(OP_ARRAY, rhs, rhs, len));
          push();
        }
      }
      else {
        /* variable rhs */
        codegen(s, t, VAL);
        gen_vmassignment(s, tree->car, rhs, val);
        if (!val) {
          pop();
        }
      }
    }
    break;

  case NODE_OP_ASGN:
    {
      mrb_sym sym = sym(tree->cdr->car);
      mrb_int len;
      const char *name = mrb_sym2name_len(s->mrb, sym, &len);
      int idx, callargs = -1, vsp = -1;

      if ((len == 2 && name[0] == '|' && name[1] == '|') &&
          ((intptr_t)tree->car->car == NODE_CONST ||
           (intptr_t)tree->car->car == NODE_CVAR)) {
        int onerr, noexc, exc;
        struct loopinfo *lp;

        onerr = genop(s, MKOP_Bx(OP_ONERR, 0));
        lp = loop_push(s, LOOP_BEGIN);
        lp->pc1 = onerr;
        exc = cursp();
        codegen(s, tree->car, VAL);
        lp->type = LOOP_RESCUE;
        genop(s, MKOP_A(OP_POPERR, 1));
        noexc = genop(s, MKOP_Bx(OP_JMP, 0));
        dispatch(s, onerr);
        genop(s, MKOP_A(OP_RESCUE, exc));
        genop(s, MKOP_A(OP_LOADF, exc));
        dispatch(s, noexc);
      }
      else if ((intptr_t)tree->car->car == NODE_CALL) {
        node *n = tree->car->cdr;

        if (val) {
          vsp = cursp();
          push();
        }
        codegen(s, n->car, VAL);   /* receiver */
        idx = new_msym(s, sym(n->cdr->car));
        if (n->cdr->cdr->car) {
          int i = gen_values(s, n->cdr->cdr->car->car, VAL);
          if (i >= 0) {
            pop_n(i);
            genop(s, MKOP_ABC(OP_ARRAY, cursp(), cursp(), i));
          }
          genop(s, MKOP_AB(OP_MOVE, cursp()+1, cursp()-1));
          genop(s, MKOP_AB(OP_MOVE, cursp()+2, cursp()));
          push();
          genop(s, MKOP_ABC(OP_SEND, cursp(), idx, CALL_MAXARGS));
          callargs = CALL_MAXARGS;
        }
        else {
          genop(s, MKOP_AB(OP_MOVE, cursp(), cursp()-1));
          genop(s, MKOP_ABC(OP_SEND, cursp(), idx, 0));
          callargs = 1;
        }
        push();
      }
      else {
        codegen(s, tree->car, VAL);
      }
      if (len == 2 &&
          ((name[0] == '|' && name[1] == '|') ||
           (name[0] == '&' && name[1] == '&'))) {
        int pos;

        pop();
        if (val) {
          pos = genop(s, MKOP_AsBx(name[0]=='|'?OP_JMPIF:OP_JMPNOT, cursp(), 0));
        }
        else {
          pos = genop_peep(s, MKOP_AsBx(name[0]=='|'?OP_JMPIF:OP_JMPNOT, cursp(), 0), NOVAL);
        }
        codegen(s, tree->cdr->cdr->car, VAL);
        pop();
        gen_assignment(s, tree->car, cursp(), val);
        dispatch(s, pos);
        return;
      }
      codegen(s, tree->cdr->cdr->car, VAL);
      push(); pop();
      pop(); pop();

      idx = new_msym(s, sym);
      if (len == 1 && name[0] == '+')  {
        genop_peep(s, MKOP_ABC(OP_ADD, cursp(), idx, 1), val);
      }
      else if (len == 1 && name[0] == '-')  {
        genop_peep(s, MKOP_ABC(OP_SUB, cursp(), idx, 1), val);
      }
      else if (len == 1 && name[0] == '*')  {
        genop(s, MKOP_ABC(OP_MUL, cursp(), idx, 1));
      }
      else if (len == 1 && name[0] == '/')  {
        genop(s, MKOP_ABC(OP_DIV, cursp(), idx, 1));
      }
      else if (len == 1 && name[0] == '<')  {
        genop(s, MKOP_ABC(OP_LT, cursp(), idx, 1));
      }
      else if (len == 2 && name[0] == '<' && name[1] == '=')  {
        genop(s, MKOP_ABC(OP_LE, cursp(), idx, 1));
      }
      else if (len == 1 && name[0] == '>')  {
        genop(s, MKOP_ABC(OP_GT, cursp(), idx, 1));
      }
      else if (len == 2 && name[0] == '>' && name[1] == '=')  {
        genop(s, MKOP_ABC(OP_GE, cursp(), idx, 1));
      }
      else {
        genop(s, MKOP_ABC(OP_SEND, cursp(), idx, 1));
      }
      if (callargs < 0) {
        gen_assignment(s, tree->car, cursp(), val);
      }
      else {
        if (callargs == CALL_MAXARGS) {
          genop(s, MKOP_AB(OP_ARYPUSH, cursp()-1, cursp()));
          if (val) {
            genop(s, MKOP_AB(OP_MOVE, vsp, cursp()));
          }
          pop();
        }
        else if (val) {
          genop(s, MKOP_AB(OP_MOVE, vsp, cursp()));
        }
        pop();
        idx = new_msym(s, attrsym(s,sym(tree->car->cdr->cdr->car)));
        genop(s, MKOP_ABC(OP_SEND, cursp(), idx, callargs));
      }
    }
    break;

  case NODE_SUPER:
    {
      int n = 0, noop = 0, sendv = 0;

      push();        /* room for receiver */
      if (tree) {
        node *args = tree->car;
        if (args) {
          n = gen_values(s, args, VAL);
          if (n < 0) {
            n = noop = sendv = 1;
            push();
          }
        }
      }
      if (tree && tree->cdr) {
        codegen(s, tree->cdr, VAL);
        pop();
      }
      else {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        push(); pop();
      }
      pop_n(n+1);
      if (sendv) n = CALL_MAXARGS;
      genop(s, MKOP_ABC(OP_SUPER, cursp(), 0, n));
      if (val) push();
    }
    break;

  case NODE_ZSUPER:
    {
      codegen_scope *s2 = s;
      int lv = 0, ainfo = 0;

      push();        /* room for receiver */
      while (!s2->mscope) {
        lv++;
        s2 = s2->prev;
        if (!s2) break;
      }
      if (s2) ainfo = s2->ainfo;
      genop(s, MKOP_ABx(OP_ARGARY, cursp(), (ainfo<<4)|(lv & 0xf)));
      push(); push(); pop();    /* ARGARY pushes two values */
      if (tree && tree->cdr) {
        codegen(s, tree->cdr, VAL);
        pop();
      }
      pop(); pop();
      genop(s, MKOP_ABC(OP_SUPER, cursp(), 0, CALL_MAXARGS));
      if (val) push();
    }
    break;

  case NODE_RETURN:
    if (tree) {
      codegen(s, tree, VAL);
      pop();
    }
    else {
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
    }
    if (s->loop) {
      genop(s, MKOP_AB(OP_RETURN, cursp(), OP_R_RETURN));
    }
    else {
      genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
    }
    if (val) push();
    break;

  case NODE_YIELD:
    {
      codegen_scope *s2 = s;
      int lv = 0, ainfo = 0;
      int n = 0, sendv = 0;

      while (!s2->mscope) {
        lv++;
        s2 = s2->prev;
        if (!s2) break;
      }
      if (s2) ainfo = s2->ainfo;
      genop(s, MKOP_ABx(OP_BLKPUSH, cursp(), (ainfo<<4)|(lv & 0xf)));
      push();
      if (tree) {
        n = gen_values(s, tree, VAL);
        if (n < 0) {
          n = sendv = 1;
          push();
        }
      }
      pop_n(n+1);
      if (sendv) n = CALL_MAXARGS;
      genop(s, MKOP_ABC(OP_SEND, cursp(), new_msym(s, mrb_intern_lit(s->mrb, "call")), n));
      if (val) push();
    }
    break;

  case NODE_BREAK:
    loop_break(s, tree);
    if (val) push();
    break;

  case NODE_NEXT:
    if (!s->loop) {
      raise_error(s, "unexpected next");
    }
    else if (s->loop->type == LOOP_NORMAL) {
      if (s->ensure_level > s->loop->ensure_level) {
        genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - s->loop->ensure_level), NOVAL);
      }
      codegen(s, tree, NOVAL);
      genop(s, MKOP_sBx(OP_JMP, s->loop->pc1 - s->pc));
    }
    else {
      if (tree) {
        codegen(s, tree, VAL);
        pop();
      }
      else {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
      }
      genop_peep(s, MKOP_AB(OP_RETURN, cursp(), OP_R_NORMAL), NOVAL);
    }
    if (val) push();
    break;

  case NODE_REDO:
    if (!s->loop) {
      raise_error(s, "unexpected redo");
    }
    else {
      if (s->ensure_level > s->loop->ensure_level) {
        genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - s->loop->ensure_level), NOVAL);
      }
      genop(s, MKOP_sBx(OP_JMP, s->loop->pc2 - s->pc));
    }
    break;

  case NODE_RETRY:
    {
      const char *msg = "unexpected retry";

      if (!s->loop) {
        raise_error(s, msg);
      }
      else {
        struct loopinfo *lp = s->loop;
        int n = 0;

        while (lp && lp->type != LOOP_RESCUE) {
          if (lp->type == LOOP_BEGIN) {
            n++;
          }
          lp = lp->prev;
        }
        if (!lp) {
          raise_error(s, msg);
        }
        else {
          if (n > 0) {
            while (n--) {
              genop_peep(s, MKOP_A(OP_POPERR, 1), NOVAL);
            }
          }
          if (s->ensure_level > lp->ensure_level) {
            genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - lp->ensure_level), NOVAL);
          }
          genop(s, MKOP_sBx(OP_JMP, lp->pc1 - s->pc));
        }
      }
    }
    break;

  case NODE_LVAR:
    if (val) {
      int idx = lv_idx(s, sym(tree));

      if (idx > 0) {
        genop_peep(s, MKOP_AB(OP_MOVE, cursp(), idx), NOVAL);
      }
      else {
        int lv = 0;
        codegen_scope *up = s->prev;

        while (up) {
          idx = lv_idx(up, sym(tree));
          if (idx > 0) {
            genop(s, MKOP_ABC(OP_GETUPVAR, cursp(), idx, lv));
            break;
          }
          lv++;
          up = up->prev;
        }
      }
      push();
    }
    break;

  case NODE_GVAR:
    if (val) {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETGLOBAL, cursp(), sym));
      push();
    }
    break;

  case NODE_IVAR:
    if (val) {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETIV, cursp(), sym));
      push();
    }
    break;

  case NODE_CVAR:
    if (val) {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETCV, cursp(), sym));
      push();
    }
    break;

  case NODE_CONST:
    {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_GETCONST, cursp(), sym));
      if (val) {
        push();
      }
    }
    break;

  case NODE_DEFINED:
    codegen(s, tree, VAL);
    break;

  case NODE_BACK_REF:
    if (val) {
      char buf[2] = { '$' };
      mrb_value str;
      int sym;

      buf[1] = (char)(intptr_t)tree;
      str = mrb_str_new(s->mrb, buf, 2);
      sym = new_sym(s, mrb_intern_str(s->mrb, str));
      genop(s, MKOP_ABx(OP_GETGLOBAL, cursp(), sym));
      push();
    }
    break;

  case NODE_NTH_REF:
    if (val) {
      int sym;
      mrb_state *mrb = s->mrb;
      mrb_value fix = mrb_fixnum_value((intptr_t)tree);
      mrb_value str = mrb_str_buf_new(mrb, 4);

      mrb_str_cat_lit(mrb, str, "$");
      mrb_str_cat_str(mrb, str, mrb_fixnum_to_str(mrb, fix, 10));
      sym = new_sym(s, mrb_intern_str(mrb, str));
      genop(s, MKOP_ABx(OP_GETGLOBAL, cursp(), sym));
      push();
    }
    break;

  case NODE_ARG:
    /* should not happen */
    break;

  case NODE_BLOCK_ARG:
    codegen(s, tree, VAL);
    break;

  case NODE_INT:
    if (val) {
      char *p = (char*)tree->car;
      int base = (intptr_t)tree->cdr->car;
      mrb_int i;
      mrb_code co;
      mrb_bool overflow;

      i = readint_mrb_int(s, p, base, FALSE, &overflow);
      if (overflow) {
        double f = readint_float(s, p, base);
        int off = new_lit(s, mrb_float_value(s->mrb, f));

        genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
      }
      else {
        if (i < MAXARG_sBx && i > -MAXARG_sBx) {
          co = MKOP_AsBx(OP_LOADI, cursp(), i);
        }
        else {
          int off = new_lit(s, mrb_fixnum_value(i));
          co = MKOP_ABx(OP_LOADL, cursp(), off);
        }
        genop(s, co);
      }
      push();
    }
    break;

  case NODE_FLOAT:
    if (val) {
      char *p = (char*)tree;
      mrb_float f = str_to_mrb_float(p);
      int off = new_lit(s, mrb_float_value(s->mrb, f));

      genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
      push();
    }
    break;

  case NODE_NEGATE:
    {
      nt = (intptr_t)tree->car;
      tree = tree->cdr;
      switch (nt) {
      case NODE_FLOAT:
        {
          char *p = (char*)tree;
          mrb_float f = str_to_mrb_float(p);
          int off = new_lit(s, mrb_float_value(s->mrb, -f));

          genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
          push();
        }
        break;

      case NODE_INT:
        {
          char *p = (char*)tree->car;
          int base = (intptr_t)tree->cdr->car;
          mrb_int i;
          mrb_code co;
          mrb_bool overflow;

          i = readint_mrb_int(s, p, base, TRUE, &overflow);
          if (overflow) {
            double f = readint_float(s, p, base);
            int off = new_lit(s, mrb_float_value(s->mrb, -f));

            genop(s, MKOP_ABx(OP_LOADL, cursp(), off));
          }
          else {
            if (i < MAXARG_sBx && i > -MAXARG_sBx) {
              co = MKOP_AsBx(OP_LOADI, cursp(), i);
            }
            else {
              int off = new_lit(s, mrb_fixnum_value(i));
              co = MKOP_ABx(OP_LOADL, cursp(), off);
            }
            genop(s, co);
          }
          push();
        }
        break;

      default:
        {
          int sym = new_msym(s, mrb_intern_lit(s->mrb, "-"));

          genop(s, MKOP_ABx(OP_LOADI, cursp(), 0));
          push();
          codegen(s, tree, VAL);
          pop(); pop();
          genop(s, MKOP_ABC(OP_SUB, cursp(), sym, 2));
        }
        break;
      }
    }
    break;

  case NODE_STR:
    if (val) {
      char *p = (char*)tree->car;
      size_t len = (intptr_t)tree->cdr;
      int ai = mrb_gc_arena_save(s->mrb);
      int off = new_lit(s, mrb_str_new(s->mrb, p, len));

      mrb_gc_arena_restore(s->mrb, ai);
      genop(s, MKOP_ABx(OP_STRING, cursp(), off));
      push();
    }
    break;

  case NODE_HEREDOC:
    tree = ((struct mrb_parser_heredoc_info *)tree)->doc;
    /* fall through */
  case NODE_DSTR:
    if (val) {
      node *n = tree;

      if (!n) break;
      codegen(s, n->car, VAL);
      n = n->cdr;
      while (n) {
        codegen(s, n->car, VAL);
        pop(); pop();
        genop_peep(s, MKOP_AB(OP_STRCAT, cursp(), cursp()+1), VAL);
        push();
        n = n->cdr;
      }
    }
    else {
      node *n = tree;

      while (n) {
        if ((intptr_t)n->car->car != NODE_STR) {
          codegen(s, n->car, NOVAL);
        }
        n = n->cdr;
      }
    }
    break;

  case NODE_WORDS:
    gen_literal_array(s, tree, FALSE, val);
    break;

  case NODE_SYMBOLS:
    gen_literal_array(s, tree, TRUE, val);
    break;

  case NODE_DXSTR:
    {
      node *n;
      int ai = mrb_gc_arena_save(s->mrb);
      int sym = new_sym(s, mrb_intern_lit(s->mrb, "Kernel"));

      if (val == NOVAL) { push(); }
      genop(s, MKOP_A(OP_OCLASS, cursp()));
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      push();
      codegen(s, tree->car, VAL);
      n = tree->cdr;
      while (n) {
        if ((intptr_t)n->car->car == NODE_XSTR) {
          n->car->car = (struct mrb_ast_node*)(intptr_t)NODE_STR;
          mrb_assert(!n->cdr); /* must be the end */
        }
        codegen(s, n->car, VAL);
        pop(); pop();
        genop_peep(s, MKOP_AB(OP_STRCAT, cursp(), cursp()+1), VAL);
        push();
        n = n->cdr;
      }
      pop();
      pop();
      sym = new_sym(s, mrb_intern_lit(s->mrb, "`"));
      genop(s, MKOP_ABC(OP_SEND, cursp(), sym, 1));
      if (val == NOVAL) { pop(); }
      else { push(); }
      mrb_gc_arena_restore(s->mrb, ai);
    }
    break;

  case NODE_XSTR:
    {
      char *p = (char*)tree->car;
      size_t len = (intptr_t)tree->cdr;
      int ai = mrb_gc_arena_save(s->mrb);
      int sym = new_sym(s, mrb_intern_lit(s->mrb, "Kernel"));
      int off = new_lit(s, mrb_str_new(s->mrb, p, len));

      if (val == NOVAL) { push(); }
      genop(s, MKOP_A(OP_OCLASS, cursp()));
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      push();
      genop(s, MKOP_ABx(OP_STRING, cursp(), off));
      pop();
      sym = new_sym(s, mrb_intern_lit(s->mrb, "`"));
      genop(s, MKOP_ABC(OP_SEND, cursp(), sym, 1));
      if (val == NOVAL) { pop(); }
      else { push(); }
      mrb_gc_arena_restore(s->mrb, ai);
    }
    break;

  case NODE_REGX:
    if (val) {
      char *p1 = (char*)tree->car;
      char *p2 = (char*)tree->cdr->car;
      char *p3 = (char*)tree->cdr->cdr;
      int ai = mrb_gc_arena_save(s->mrb);
      int sym = new_sym(s, mrb_intern_lit(s->mrb, REGEXP_CLASS));
      int off = new_lit(s, mrb_str_new_cstr(s->mrb, p1));
      int argc = 1;

      genop(s, MKOP_A(OP_OCLASS, cursp()));
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      push();
      genop(s, MKOP_ABx(OP_STRING, cursp(), off));
      if (p2 || p3) {
        push();
        if (p2) {
          off = new_lit(s, mrb_str_new_cstr(s->mrb, p2));
          genop(s, MKOP_ABx(OP_STRING, cursp(), off));
        } else {
          genop(s, MKOP_A(OP_LOADNIL, cursp()));
        }
        argc++;
        if (p3) {
          push();
          off = new_lit(s, mrb_str_new(s->mrb, p3, 1));
          genop(s, MKOP_ABx(OP_STRING, cursp(), off));
          argc++;
          pop();
        }
        pop();
      }
      pop();
      sym = new_sym(s, mrb_intern_lit(s->mrb, "compile"));
      genop(s, MKOP_ABC(OP_SEND, cursp(), sym, argc));
      mrb_gc_arena_restore(s->mrb, ai);
      push();
    }
    break;

  case NODE_DREGX:
    if (val) {
      node *n = tree->car;
      int ai = mrb_gc_arena_save(s->mrb);
      int sym = new_sym(s, mrb_intern_lit(s->mrb, REGEXP_CLASS));
      int argc = 1;
      int off;
      char *p;

      genop(s, MKOP_A(OP_OCLASS, cursp()));
      genop(s, MKOP_ABx(OP_GETMCNST, cursp(), sym));
      push();
      codegen(s, n->car, VAL);
      n = n->cdr;
      while (n) {
        codegen(s, n->car, VAL);
        pop(); pop();
        genop_peep(s, MKOP_AB(OP_STRCAT, cursp(), cursp()+1), VAL);
        push();
        n = n->cdr;
      }
      n = tree->cdr->cdr;
      if (n->car) {
        p = (char*)n->car;
        off = new_lit(s, mrb_str_new_cstr(s->mrb, p));
        codegen(s, tree->car, VAL);
        genop(s, MKOP_ABx(OP_STRING, cursp(), off));
        pop();
        genop_peep(s, MKOP_AB(OP_STRCAT, cursp(), cursp()+1), VAL);
      }
      if (n->cdr) {
        char *p2 = (char*)n->cdr;

        push();
        off = new_lit(s, mrb_str_new_cstr(s->mrb, p2));
        genop(s, MKOP_ABx(OP_STRING, cursp(), off));
        argc++;
        pop();
      }
      pop();
      sym = new_sym(s, mrb_intern_lit(s->mrb, "compile"));
      genop(s, MKOP_ABC(OP_SEND, cursp(), sym, argc));
      mrb_gc_arena_restore(s->mrb, ai);
      push();
    }
    else {
      node *n = tree->car;

      while (n) {
        if ((intptr_t)n->car->car != NODE_STR) {
          codegen(s, n->car, NOVAL);
        }
        n = n->cdr;
      }
    }
    break;

  case NODE_SYM:
    if (val) {
      int sym = new_sym(s, sym(tree));

      genop(s, MKOP_ABx(OP_LOADSYM, cursp(), sym));
      push();
    }
    break;

  case NODE_DSYM:
    codegen(s, tree, val);
    if (val) {
      gen_send_intern(s);
    }
    break;

  case NODE_SELF:
    if (val) {
      genop(s, MKOP_A(OP_LOADSELF, cursp()));
      push();
    }
    break;

  case NODE_NIL:
    if (val) {
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      push();
    }
    break;

  case NODE_TRUE:
    if (val) {
      genop(s, MKOP_A(OP_LOADT, cursp()));
      push();
    }
    break;

  case NODE_FALSE:
    if (val) {
      genop(s, MKOP_A(OP_LOADF, cursp()));
      push();
    }
    break;

  case NODE_ALIAS:
    {
      int a = new_msym(s, sym(tree->car));
      int b = new_msym(s, sym(tree->cdr));
      int c = new_msym(s, mrb_intern_lit(s->mrb, "alias_method"));

      genop(s, MKOP_A(OP_TCLASS, cursp()));
      push();
      genop(s, MKOP_ABx(OP_LOADSYM, cursp(), a));
      push();
      genop(s, MKOP_ABx(OP_LOADSYM, cursp(), b));
      push();
      genop(s, MKOP_A(OP_LOADNIL, cursp()));
      pop_n(3);
      genop(s, MKOP_ABC(OP_SEND, cursp(), c, 2));
      if (val) {
        push();
      }
    }
   break;

  case NODE_UNDEF:
    {
      int undef = new_msym(s, mrb_intern_lit(s->mrb, "undef_method"));
      int num = 0;
      node *t = tree;

      genop(s, MKOP_A(OP_TCLASS, cursp()));
      push();
      while (t) {
        int symbol = new_msym(s, sym(t->car));
        genop(s, MKOP_ABx(OP_LOADSYM, cursp(), symbol));
        push();
        t = t->cdr;
        num++;
      }
      pop_n(num + 1);
      genop(s, MKOP_ABC(OP_SEND, cursp(), undef, num));
      if (val) {
        push();
      }
    }
    break;

  case NODE_CLASS:
    {
      int idx;

      if (tree->car->car == (node*)0) {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        push();
      }
      else if (tree->car->car == (node*)1) {
        genop(s, MKOP_A(OP_OCLASS, cursp()));
        push();
      }
      else {
        codegen(s, tree->car->car, VAL);
      }
      if (tree->cdr->car) {
        codegen(s, tree->cdr->car, VAL);
      }
      else {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        push();
      }
      pop(); pop();
      idx = new_msym(s, sym(tree->car->cdr));
      genop(s, MKOP_AB(OP_CLASS, cursp(), idx));
      idx = scope_body(s, tree->cdr->cdr->car, val);
      genop(s, MKOP_ABx(OP_EXEC, cursp(), idx));
      if (val) {
        push();
      }
    }
    break;

  case NODE_MODULE:
    {
      int idx;

      if (tree->car->car == (node*)0) {
        genop(s, MKOP_A(OP_LOADNIL, cursp()));
        push();
      }
      else if (tree->car->car == (node*)1) {
        genop(s, MKOP_A(OP_OCLASS, cursp()));
        push();
      }
      else {
        codegen(s, tree->car->car, VAL);
      }
      pop();
      idx = new_msym(s, sym(tree->car->cdr));
      genop(s, MKOP_AB(OP_MODULE, cursp(), idx));
      idx = scope_body(s, tree->cdr->car, val);
      genop(s, MKOP_ABx(OP_EXEC, cursp(), idx));
      if (val) {
        push();
      }
    }
    break;

  case NODE_SCLASS:
    {
      int idx;

      codegen(s, tree->car, VAL);
      pop();
      genop(s, MKOP_AB(OP_SCLASS, cursp(), cursp()));
      idx = scope_body(s, tree->cdr->car, val);
      genop(s, MKOP_ABx(OP_EXEC, cursp(), idx));
      if (val) {
        push();
      }
    }
    break;

  case NODE_DEF:
    {
      int sym = new_msym(s, sym(tree->car));
      int idx = lambda_body(s, tree->cdr, 0);

      genop(s, MKOP_A(OP_TCLASS, cursp()));
      push();
      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_METHOD));
      push(); pop();
      pop();
      genop(s, MKOP_AB(OP_METHOD, cursp(), sym));
      if (val) {
        genop(s, MKOP_ABx(OP_LOADSYM, cursp(), sym));
        push();
      }
    }
    break;

  case NODE_SDEF:
    {
      node *recv = tree->car;
      int sym = new_msym(s, sym(tree->cdr->car));
      int idx = lambda_body(s, tree->cdr->cdr, 0);

      codegen(s, recv, VAL);
      pop();
      genop(s, MKOP_AB(OP_SCLASS, cursp(), cursp()));
      push();
      genop(s, MKOP_Abc(OP_LAMBDA, cursp(), idx, OP_L_METHOD));
      pop();
      genop(s, MKOP_AB(OP_METHOD, cursp(), sym));
      if (val) {
        genop(s, MKOP_ABx(OP_LOADSYM, cursp(), sym));
        push();
      }
    }
    break;

  case NODE_POSTEXE:
    codegen(s, tree, NOVAL);
    break;

  default:
    break;
  }
}

static void
scope_add_irep(codegen_scope *s, mrb_irep *irep)
{
  if (s->irep == NULL) {
    s->irep = irep;
    return;
  }
  if (s->irep->rlen == s->rcapa) {
    s->rcapa *= 2;
    s->irep->reps = (mrb_irep**)codegen_realloc(s, s->irep->reps, sizeof(mrb_irep*)*s->rcapa);
  }
  s->irep->reps[s->irep->rlen] = irep;
  s->irep->rlen++;
}

static codegen_scope*
scope_new(mrb_state *mrb, codegen_scope *prev, node *lv)
{
  static const codegen_scope codegen_scope_zero = { 0 };
  mrb_pool *pool = mrb_pool_open(mrb);
  codegen_scope *p = (codegen_scope *)mrb_pool_alloc(pool, sizeof(codegen_scope));

  if (!p) return NULL;
  *p = codegen_scope_zero;
  p->mrb = mrb;
  p->mpool = pool;
  if (!prev) return p;
  p->prev = prev;
  p->ainfo = -1;
  p->mscope = 0;

  p->irep = mrb_add_irep(mrb);
  scope_add_irep(prev, p->irep);

  p->rcapa = 8;
  p->irep->reps = (mrb_irep**)mrb_malloc(mrb, sizeof(mrb_irep*)*p->rcapa);

  p->icapa = 1024;
  p->iseq = (mrb_code*)mrb_malloc(mrb, sizeof(mrb_code)*p->icapa);
  p->irep->iseq = p->iseq;

  p->pcapa = 32;
  p->irep->pool = (mrb_value*)mrb_malloc(mrb, sizeof(mrb_value)*p->pcapa);
  p->irep->plen = 0;

  p->scapa = 256;
  p->irep->syms = (mrb_sym*)mrb_malloc(mrb, sizeof(mrb_sym)*p->scapa);
  p->irep->slen = 0;

  p->lv = lv;
  p->sp += node_len(lv)+1;        /* add self */
  p->nlocals = p->sp;
  if (lv) {
    node *n = lv;
    size_t i = 0;

    p->irep->lv = (struct mrb_locals*)mrb_malloc(mrb, sizeof(struct mrb_locals) * (p->nlocals - 1));
    for (i=0, n=lv; n; i++,n=n->cdr) {
      p->irep->lv[i].name = lv_name(n);
      if (lv_name(n)) {
        p->irep->lv[i].r = lv_idx(p, lv_name(n));
      }
      else {
        p->irep->lv[i].r = 0;
      }
    }
    mrb_assert(i + 1 == p->nlocals);
  }
  p->ai = mrb_gc_arena_save(mrb);

  p->filename = prev->filename;
  if (p->filename) {
    p->lines = (uint16_t*)mrb_malloc(mrb, sizeof(short)*p->icapa);
  }
  p->lineno = prev->lineno;

  /* debug setting */
  p->debug_start_pos = 0;
  if (p->filename) {
    mrb_debug_info_alloc(mrb, p->irep);
    p->irep->filename = p->filename;
    p->irep->lines = p->lines;
  }
  else {
    p->irep->debug_info = NULL;
  }
  p->parser = prev->parser;
  p->filename_index = prev->filename_index;

  return p;
}

static void
scope_finish(codegen_scope *s)
{
  mrb_state *mrb = s->mrb;
  mrb_irep *irep = s->irep;
  size_t fname_len;
  char *fname;

  irep->flags = 0;
  if (s->iseq) {
    irep->iseq = (mrb_code *)codegen_realloc(s, s->iseq, sizeof(mrb_code)*s->pc);
    irep->ilen = s->pc;
    if (s->lines) {
      irep->lines = (uint16_t *)codegen_realloc(s, s->lines, sizeof(uint16_t)*s->pc);
    }
    else {
      irep->lines = 0;
    }
  }
  irep->pool = (mrb_value*)codegen_realloc(s, irep->pool, sizeof(mrb_value)*irep->plen);
  irep->syms = (mrb_sym*)codegen_realloc(s, irep->syms, sizeof(mrb_sym)*irep->slen);
  irep->reps = (mrb_irep**)codegen_realloc(s, irep->reps, sizeof(mrb_irep*)*irep->rlen);
  if (s->filename) {
    s->irep->filename = mrb_parser_get_filename(s->parser, s->filename_index);
    mrb_debug_info_append_file(mrb, s->irep, s->debug_start_pos, s->pc);

    fname_len = strlen(s->filename);
    fname = (char*)codegen_malloc(s, fname_len + 1);
    memcpy(fname, s->filename, fname_len);
    fname[fname_len] = '\0';
    irep->filename = fname;
  }

  irep->nlocals = s->nlocals;
  irep->nregs = s->nregs;

  mrb_gc_arena_restore(mrb, s->ai);
  mrb_pool_close(s->mpool);
}

static struct loopinfo*
loop_push(codegen_scope *s, enum looptype t)
{
  struct loopinfo *p = (struct loopinfo *)codegen_palloc(s, sizeof(struct loopinfo));

  p->type = t;
  p->pc1 = p->pc2 = p->pc3 = 0;
  p->prev = s->loop;
  p->ensure_level = s->ensure_level;
  p->acc = cursp();
  s->loop = p;

  return p;
}

static void
loop_break(codegen_scope *s, node *tree)
{
  if (!s->loop) {
    codegen(s, tree, NOVAL);
    raise_error(s, "unexpected break");
  }
  else {
    struct loopinfo *loop;

    if (tree) {
      codegen(s, tree, VAL);
      pop();
    }

    loop = s->loop;
    while (loop && loop->type == LOOP_BEGIN) {
      genop_peep(s, MKOP_A(OP_POPERR, 1), NOVAL);
      loop = loop->prev;
    }
    while (loop && loop->type == LOOP_RESCUE) {
      loop = loop->prev;
    }
    if (!loop) {
      raise_error(s, "unexpected break");
      return;
    }

    if (loop->type == LOOP_NORMAL) {
      int tmp;

      if (s->ensure_level > s->loop->ensure_level) {
        genop_peep(s, MKOP_A(OP_EPOP, s->ensure_level - s->loop->ensure_level), NOVAL);
      }
      if (tree) {
        genop_peep(s, MKOP_AB(OP_MOVE, loop->acc, cursp()), NOVAL);
      }
      tmp = genop(s, MKOP_sBx(OP_JMP, loop->pc3));
      loop->pc3 = tmp;
    }
    else {
      genop(s, MKOP_AB(OP_RETURN, cursp(), OP_R_BREAK));
    }
  }
}

static void
loop_pop(codegen_scope *s, int val)
{
  if (val) {
    genop(s, MKOP_A(OP_LOADNIL, cursp()));
  }
  dispatch_linked(s, s->loop->pc3);
  s->loop = s->loop->prev;
  if (val) push();
}

MRB_API struct RProc*
mrb_generate_code(mrb_state *mrb, parser_state *p)
{
  codegen_scope *scope = scope_new(mrb, 0, 0);
  struct RProc *proc;

  if (!scope) {
    return NULL;
  }
  scope->mrb = mrb;
  scope->parser = p;
  scope->filename = p->filename;
  scope->filename_index = p->current_filename_index;

  MRB_TRY(&scope->jmp) {
    /* prepare irep */
    codegen(scope, p->tree, NOVAL);
    proc = mrb_proc_new(mrb, scope->irep);
    mrb_irep_decref(mrb, scope->irep);
    mrb_pool_close(scope->mpool);
    return proc;
  }
  MRB_CATCH(&scope->jmp) {
    if (scope->filename == scope->irep->filename) {
      scope->irep->filename = NULL;
    }
    mrb_irep_decref(mrb, scope->irep);
    mrb_pool_close(scope->mpool);
    return NULL;
  }
  MRB_END_EXC(&scope->jmp);
}
