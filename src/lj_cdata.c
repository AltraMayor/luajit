/*
** C data management.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#include "lj_obj.h"

#if LJ_HASFFI

#include <assert.h>

#include "lauxlib.h"

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_state.h"

/* -- C data allocation --------------------------------------------------- */

/* Allocate a new C data object holding a reference to another object. */
GCcdata *lj_cdata_newref(CTState *cts, const void *p, CTypeID id)
{
  CTypeID refid = lj_ctype_intern(cts, CTINFO_REF(id), CTSIZE_PTR);
  GCcdata *cd = lj_cdata_new(cts, refid, CTSIZE_PTR);
  *(const void **)cdataptr(cd) = p;
  return cd;
}

/* Allocate variable-sized or specially aligned C data object. */
GCcdata *lj_cdata_newv(CTState *cts, CTypeID id, CTSize sz, CTSize align)
{
  global_State *g;
  MSize extra = sizeof(GCcdataVar) + sizeof(GCcdata) +
		(align > CT_MEMALIGN ? (1u<<align) - (1u<<CT_MEMALIGN) : 0);
  char *p = lj_mem_newt(cts->L, extra + sz, char);
  uintptr_t adata = (uintptr_t)p + sizeof(GCcdataVar) + sizeof(GCcdata);
  uintptr_t almask = (1u << align) - 1u;
  GCcdata *cd = (GCcdata *)(((adata + almask) & ~almask) - sizeof(GCcdata));
  lua_assert((char *)cd - p < 65536);
  cdatav(cd)->offset = (uint16_t)((char *)cd - p);
  cdatav(cd)->extra = extra;
  cdatav(cd)->len = sz;
  g = cts->g;
  setgcrefr(cd->nextgc, g->gc.root);
  setgcref(g->gc.root, obj2gco(cd));
  newwhite(g, obj2gco(cd));
  cd->marked |= 0x80;
  cd->gct = ~LJ_TCDATA;
  cd->ctypeid = id;
  return cd;
}

/* Free a C data object. */
void LJ_FASTCALL lj_cdata_free(global_State *g, GCcdata *cd)
{
  if (LJ_UNLIKELY(cd->marked & LJ_GC_CDATA_FIN)) {
    GCobj *root;
    makewhite(g, obj2gco(cd));
    markfinalized(obj2gco(cd));
    if ((root = gcref(g->gc.mmudata)) != NULL) {
      setgcrefr(cd->nextgc, root->gch.nextgc);
      setgcref(root->gch.nextgc, obj2gco(cd));
      setgcref(g->gc.mmudata, obj2gco(cd));
    } else {
      setgcref(cd->nextgc, obj2gco(cd));
      setgcref(g->gc.mmudata, obj2gco(cd));
    }
  } else if (LJ_LIKELY(!cdataisv(cd))) {
    CType *ct = ctype_raw(ctype_ctsG(g), cd->ctypeid);
    CTSize sz = ctype_hassize(ct->info) ? ct->size : CTSIZE_PTR;
    lua_assert(ctype_hassize(ct->info) || ctype_isfunc(ct->info) ||
	       ctype_isextern(ct->info));
    lj_mem_free(g, cd, sizeof(GCcdata) + sz);
  } else {
    lj_mem_free(g, memcdatav(cd), sizecdatav(cd));
  }
}

TValue * LJ_FASTCALL lj_cdata_setfin(lua_State *L, GCcdata *cd)
{
  global_State *g = G(L);
  GCtab *t = ctype_ctsG(g)->finalizer;
  if (gcref(t->metatable)) {
    /* Add cdata to finalizer table, if still enabled. */
    TValue *tv, tmp;
    setcdataV(L, &tmp, cd);
    lj_gc_anybarriert(L, t);
    tv = lj_tab_set(L, t, &tmp);
    cd->marked |= LJ_GC_CDATA_FIN;
    return tv;
  } else {
    /* Otherwise return dummy TValue. */
    return &g->tmptv;
  }
}

/* -- C data indexing ----------------------------------------------------- */

/* Index C data by a TValue. Return CType and pointer. */
CType *lj_cdata_index(CTState *cts, GCcdata *cd, cTValue *key, uint8_t **pp,
		      CTInfo *qual)
{
  uint8_t *p = (uint8_t *)cdataptr(cd);
  CType *ct = ctype_get(cts, cd->ctypeid);
  ptrdiff_t idx;

  /* Resolve reference for cdata object. */
  if (ctype_isref(ct->info)) {
    lua_assert(ct->size == CTSIZE_PTR);
    p = *(uint8_t **)p;
    ct = ctype_child(cts, ct);
  }

collect_attrib:
  /* Skip attributes and collect qualifiers. */
  while (ctype_isattrib(ct->info)) {
    if (ctype_attrib(ct->info) == CTA_QUAL) *qual |= ct->size;
    ct = ctype_child(cts, ct);
  }
  lua_assert(!ctype_isref(ct->info));  /* Interning rejects refs to refs. */

  if (tvisint(key)) {
    idx = (ptrdiff_t)intV(key);
    goto integer_key;
  } else if (tvisnum(key)) {  /* Numeric key. */
    idx = LJ_64 ? (ptrdiff_t)numV(key) : (ptrdiff_t)lj_num2int(numV(key));
  integer_key:
    if (ctype_ispointer(ct->info)) {
      CTSize sz = lj_ctype_size(cts, ctype_cid(ct->info));  /* Element size. */
      if (sz == CTSIZE_INVALID)
	lj_err_caller(cts->L, LJ_ERR_FFI_INVSIZE);
      if (ctype_isptr(ct->info)) {
	p = (uint8_t *)cdata_getptr(p, ct->size);
      } else if ((ct->info & (CTF_VECTOR|CTF_COMPLEX))) {
	if ((ct->info & CTF_COMPLEX)) idx &= 1;
	*qual |= CTF_CONST;  /* Valarray elements are constant. */
      }
      *pp = p + idx*(int32_t)sz;
      return ct;
    }
  } else if (tviscdata(key)) {  /* Integer cdata key. */
    GCcdata *cdk = cdataV(key);
    CType *ctk = ctype_raw(cts, cdk->ctypeid);
    if (ctype_isenum(ctk->info)) ctk = ctype_child(cts, ctk);
    if (ctype_isinteger(ctk->info)) {
      lj_cconv_ct_ct(cts, ctype_get(cts, CTID_INT_PSZ), ctk,
		     (uint8_t *)&idx, cdataptr(cdk), 0);
      goto integer_key;
    }
  } else if (tvisstr(key)) {  /* String key. */
    GCstr *name = strV(key);
    if (ctype_isstruct(ct->info)) {
      CTSize ofs;
      CType *fct = lj_ctype_getfieldq(cts, ct, name, &ofs, qual);
      if (fct) {
	*pp = p + ofs;
	return fct;
      }
    } else if (ctype_iscomplex(ct->info)) {
      if (name->len == 2) {
	*qual |= CTF_CONST;  /* Complex fields are constant. */
	if (strdata(name)[0] == 'r' && strdata(name)[1] == 'e') {
	  *pp = p;
	  return ct;
	} else if (strdata(name)[0] == 'i' && strdata(name)[1] == 'm') {
	  *pp = p + (ct->size >> 1);
	  return ct;
	}
      }
    } else if (cd->ctypeid == CTID_CTYPEID) {
      /* Allow indexing a (pointer to) struct constructor to get constants. */
      CType *sct = ctype_raw(cts, *(CTypeID *)p);
      if (ctype_isptr(sct->info))
	sct = ctype_rawchild(cts, sct);
      if (ctype_isstruct(sct->info)) {
	CTSize ofs;
	CType *fct = lj_ctype_getfield(cts, sct, name, &ofs);
	if (fct && ctype_isconstval(fct->info))
	  return fct;
      }
      ct = sct;  /* Allow resolving metamethods for constructors, too. */
    }
  }
  if (ctype_isptr(ct->info)) {  /* Automatically perform '->'. */
    if (ctype_isstruct(ctype_rawchild(cts, ct)->info)) {
      p = (uint8_t *)cdata_getptr(p, ct->size);
      ct = ctype_child(cts, ct);
      goto collect_attrib;
    }
  }
  *qual |= 1;  /* Lookup failed. */
  return ct;  /* But return the resolved raw type. */
}

/* -- C data getters ------------------------------------------------------ */

/* Get constant value and convert to TValue. */
static void cdata_getconst(CTState *cts, TValue *o, CType *ct)
{
  CType *ctt = ctype_child(cts, ct);
  lua_assert(ctype_isinteger(ctt->info) && ctt->size <= 4);
  /* Constants are already zero-extended/sign-extended to 32 bits. */
  if ((ctt->info & CTF_UNSIGNED) && (int32_t)ct->size < 0)
    setnumV(o, (lua_Number)(uint32_t)ct->size);
  else
    setintV(o, (int32_t)ct->size);
}

/* Get C data value and convert to TValue. */
int lj_cdata_get(CTState *cts, CType *s, TValue *o, uint8_t *sp)
{
  CTypeID sid;

  if (ctype_isconstval(s->info)) {
    cdata_getconst(cts, o, s);
    return 0;  /* No GC step needed. */
  } else if (ctype_isbitfield(s->info)) {
    return lj_cconv_tv_bf(cts, s, o, sp);
  }

  /* Get child type of pointer/array/field. */
  lua_assert(ctype_ispointer(s->info) || ctype_isfield(s->info));
  sid = ctype_cid(s->info);
  s = ctype_get(cts, sid);

  /* Resolve reference for field. */
  if (ctype_isref(s->info)) {
    lua_assert(s->size == CTSIZE_PTR);
    sp = *(uint8_t **)sp;
    sid = ctype_cid(s->info);
    s = ctype_get(cts, sid);
  }

  /* Skip attributes. */
  while (ctype_isattrib(s->info))
    s = ctype_child(cts, s);

  return lj_cconv_tv_ct(cts, s, sid, o, sp);
}

/* -- C data setters ------------------------------------------------------ */

/* Convert TValue and set C data value. */
void lj_cdata_set(CTState *cts, CType *d, uint8_t *dp, TValue *o, CTInfo qual)
{
  if (ctype_isconstval(d->info)) {
    goto err_const;
  } else if (ctype_isbitfield(d->info)) {
    if (((d->info|qual) & CTF_CONST)) goto err_const;
    lj_cconv_bf_tv(cts, d, dp, o);
    return;
  }

  /* Get child type of pointer/array/field. */
  lua_assert(ctype_ispointer(d->info) || ctype_isfield(d->info));
  d = ctype_child(cts, d);

  /* Resolve reference for field. */
  if (ctype_isref(d->info)) {
    lua_assert(d->size == CTSIZE_PTR);
    dp = *(uint8_t **)dp;
    d = ctype_child(cts, d);
  }

  /* Skip attributes and collect qualifiers. */
  for (;;) {
    if (ctype_isattrib(d->info)) {
      if (ctype_attrib(d->info) == CTA_QUAL) qual |= d->size;
    } else {
      break;
    }
    d = ctype_child(cts, d);
  }

  lua_assert(ctype_hassize(d->info) && !ctype_isvoid(d->info));

  if (((d->info|qual) & CTF_CONST)) {
  err_const:
    lj_err_caller(cts->L, LJ_ERR_FFI_WRCONST);
  }

  lj_cconv_ct_tv(cts, d, dp, o, 0);
}

/* -- Gatekeeper functions ------------------------------------------------ */

/*
 * The functions below are based on Roman Tsisyk's gist:
 * An example how to work with CDATA (LuaJIT FFI) objects using lua_State
 * https://gist.github.com/rtsisyk/6103290
 *
 * The code has been changed to compile as part of LuaJIT, to comply to
 * DPDK coding style, and to match the needs of Gatekeeper.
 */

LUALIB_API void *
luaL_pushcdata(struct lua_State *l, uint32_t ctypeid, uint32_t size)
{
	CTState *cts = ctype_cts(l);
	CType *ct = ctype_raw(cts, ctypeid);
	CTSize sz;
	GCcdata *cd;
	TValue *o;

	/* ctypeid actually is CTypeID type.
	 * We don't use CTypeID type outside this file in order to
	 * avoid having to add an internal header of luajit.
	 */
	static_assert(sizeof(ctypeid) == sizeof(CTypeID),
		"sizeof(ctypeid) != sizeof(CTypeID)");

	lj_ctype_info(cts, ctypeid, &sz);
	cd = lj_cdata_new(cts, ctypeid, size);
	o = l->top;
	setcdataV(l, o, cd);
	lj_cconv_ct_init(cts, ct, sz, (uint8_t *) cdataptr(cd), o, 0);
	incr_top(l);
	return cdataptr(cd);
}

LUALIB_API void *
luaL_checkcdata(struct lua_State *l, int idx, uint32_t *ctypeid,
	const char *ctypename)
{
	GCcdata *cd;

	/* Calculate absolute value in the stack. */
	if (idx < 0)
		idx = lua_gettop(l) + idx + 1;

	if (lua_type(l, idx) != LUA_TCDATA) {
		luaL_error(l, "expected cdata `%s' as argument #%d",
			ctypename, idx);
		return NULL;
	}

	cd = cdataV(l->base + idx - 1);
	*ctypeid = cd->ctypeid;
	return (void *)cdataptr(cd);
}

LUALIB_API uint32_t
luaL_get_ctypeid(struct lua_State *l, const char *ctypename)
{
	int idx = lua_gettop(l);
	CTypeID ctypeid;
	GCcdata *cd;

	/* Get a reference to ffi.typeof. */
	luaL_loadstring(l, "return require('ffi').typeof");
	lua_call(l, 0, 1);
	if (!lua_isfunction(l, -1))
		luaL_error(l,
			"%s: can't get a reference to ffi.typeof", __func__);

	lua_pushstring(l, ctypename);

	if (lua_pcall(l, 1, 1, 0)) {
		if (lua_isstring(l, -1))
			lua_error(l);
		goto fail;
	}

	/* Returned type should be LUA_TCDATA. */
	if (lua_type(l, -1) != LUA_TCDATA)
		goto fail;
	cd = cdataV(l->base + lua_gettop(l) - 1);
	ctypeid = cd->ctypeid == CTID_CTYPEID
		? *(CTypeID *)cdataptr(cd)
		: cd->ctypeid;

	lua_settop(l, idx);
	return ctypeid;

fail:
	return luaL_error(l, "Lua call to ffi.typeof failed");
}

#endif
