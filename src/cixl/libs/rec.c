#include <ctype.h>
#include <string.h>

#include "cixl/box.h"
#include "cixl/cx.h"
#include "cixl/error.h"
#include "cixl/eval.h"
#include "cixl/libs/rec.h"
#include "cixl/parse.h"
#include "cixl/scope.h"
#include "cixl/tok.h"
#include "cixl/types/file.h"
#include "cixl/types/fimp.h"
#include "cixl/types/func.h"
#include "cixl/types/rec.h"

static bool rec_parse(struct cx *cx, FILE *in, struct cx_vec *out) {
  struct cx_vec toks;
  cx_vec_init(&toks, sizeof(struct cx_tok));
  bool ok = false;
  
  if (!cx_parse_tok(cx, in, &toks, false)) {
    cx_error(cx, cx->row, cx->col, "Missing rec id");
    goto exit1;
  }

  struct cx_tok id = *(struct cx_tok *)cx_vec_pop(&toks);
  struct cx_type *type = NULL;

  if (id.type == CX_TTYPE()) {
    type = id.as_ptr;
    
    if (!cx_is(type, cx->rec_type)) {
      cx_error(cx, id.row, id.col, "Attempt to redefine %s as rec", type->id);
      goto exit2;
    }
  }

  if (id.type != CX_TID() && id.type != CX_TTYPE()) {
    cx_error(cx, id.row, id.col, "Invalid rec id");
    goto exit2;
  }

  if (id.type == CX_TID()) {
    char *s = id.as_ptr;

    if (!isupper(s[0])) {
      cx_error(cx, id.row, id.col, "Invalid rec id: %s", s);
      goto exit2;
    }
  }

  if (!cx_parse_tok(cx, in, &toks, false)) {
    cx_error(cx, cx->row, cx->col, "Missing rec parents");
    goto exit2;
  }

  struct cx_tok parents = *(struct cx_tok *)cx_vec_pop(&toks);

  if (parents.type != CX_TGROUP()) {
    cx_error(cx, parents.row, parents.col, "Invalid rec parents");
    goto exit3;
  }

  cx_do_vec(&parents.as_vec, struct cx_tok, t) {
    if (t->type != CX_TTYPE()) {
      cx_error(cx, t->row, t->col, "Invalid rec parent");
      goto exit3;
    }

    struct cx_type *tt = t->as_ptr;
    
    if (!tt->trait && !cx_is(tt, cx->rec_type)) {
      cx_error(cx, t->row, t->col, "Invalid rec parent: %s", tt->id);
      goto exit3;
    }
  }
  
  if (!cx_parse_end(cx, in, &toks, false)) {
    if (!cx->errors.count) { cx_error(cx, cx->row, cx->col, "Missing rec end"); }
    goto exit3;
  }

  struct cx_rec_type *rec_type = type
    ? cx_baseof(type, struct cx_rec_type, imp)
    : cx_test(cx_add_rec_type(cx, type ? type->id : id.as_ptr));

  if (type) { cx_rec_type_reinit(rec_type); }

  struct cx_vec fids, ftypes;
  cx_vec_init(&fids, sizeof(struct cx_tok));
  cx_vec_init(&ftypes, sizeof(struct cx_type *));

  bool push_type(struct cx_type *t, int row, int col) {
    if (fids.count == ftypes.count) {
      cx_error(cx, row, col, "Missing field for type: %s", t->id);
      return false;
    }

    for (size_t i = ftypes.count; i < fids.count; i++) {
      *(struct cx_type **)cx_vec_push(&ftypes) = t;
    }

    return true;
  }
  
  cx_do_vec(&toks, struct cx_tok, t) {
    if (t->type == CX_TID()) {
      char *s = t->as_ptr;
      
      if (isupper(s[0])) {
	if (strcmp(s, id.as_ptr) != 0) {
	  cx_error(cx, t->row, t->col, "Invalid field type: %s", s);
	  goto exit4;
	}
	
	if (!push_type(&rec_type->imp, t->row, t->col)) { goto exit4; }
      } else {
	*(struct cx_tok *)cx_vec_push(&fids) = *t;
      }
    } else if (t->type == CX_TTYPE()) {
      if (!push_type(t->as_ptr, t->row, t->col)) { goto exit4; }
    }
  }

  if (fids.count > ftypes.count) { push_type(cx->any_type, -1, -1); }

  for (size_t i=0; i < fids.count; i++) {
    struct cx_tok *fid = cx_vec_get(&fids, i);
    struct cx_type **type = cx_vec_get(&ftypes, i);

    if (!cx_add_field(rec_type, cx_sym(cx, fid->as_ptr), *type, false)) {
      goto exit4;
    }
  }

  cx_do_vec(&parents.as_vec, struct cx_tok, t) {
    cx_derive_rec(rec_type, t->as_ptr);
  }
  
  ok = true;
 exit4:
  cx_vec_deinit(&fids);	      
  cx_vec_deinit(&ftypes);	      
 exit3:
  cx_tok_deinit(&parents);
 exit2:
  cx_tok_deinit(&id);
 exit1: {
    cx_do_vec(&toks, struct cx_tok, t) { cx_tok_deinit(t); }
    cx_vec_deinit(&toks);
    return ok;
  }
}

static bool get_imp(struct cx_scope *scope) {
  struct cx *cx = scope->cx;
  struct cx_sym f = cx_test(cx_pop(scope, false))->as_sym;
  struct cx_box r = *cx_test(cx_pop(scope, false));
  struct cx_rec_type *rt = cx_baseof(r.type, struct cx_rec_type, imp);
  bool ok = false;
  
  if (!cx_set_get(&rt->fields, &f)) {
    cx_error(cx, cx->row, cx->col, "Invalid %s field: %s", rt->imp.id, f.id);
    goto exit;
  }
  
  struct cx_box *v = cx_rec_get(r.as_ptr, f);

  if (v) {
    cx_copy(cx_push(scope), v);
  } else {
    cx_box_init(cx_push(scope), cx->nil_type);
  }

  ok = true;
 exit:
  cx_box_deinit(&r);
  return ok;
}

static bool put_imp(struct cx_scope *scope) {
  struct cx *cx = scope->cx;
  struct cx_box v = *cx_test(cx_pop(scope, false));
  struct cx_sym fid = cx_test(cx_pop(scope, false))->as_sym;
  struct cx_box r = *cx_test(cx_pop(scope, false));
  struct cx_rec_type *rt = cx_baseof(r.type, struct cx_rec_type, imp);
  struct cx_field *f = cx_set_get(&rt->fields, &fid);
  bool ok = false;
    
  if (!f) {
    cx_error(cx, cx->row, cx->col, "Invalid %s field: %s", rt->imp.id, fid.id);
    goto exit;
  }

  if (!cx_is(v.type, f->type)) {
    cx_error(cx, cx->row, cx->col, "Expected %s, was %s", f->type->id, v.type->id);
    goto exit;
  }

  cx_rec_put(r.as_ptr, fid, &v);
  ok = true;
 exit:
  cx_box_deinit(&v);
  cx_box_deinit(&r);
  return ok;
}

static bool eqval_imp(struct cx_scope *scope) {
  struct cx *cx = scope->cx;
  
  struct cx_box
    x = *cx_test(cx_pop(scope, false)),
    y = *cx_test(cx_pop(scope, false));

  struct cx_rec *xr = x.as_ptr, *yr = y.as_ptr;
  bool eqval = true;
  
  if (xr->values.members.count != yr->values.members.count) {
    eqval = false;
  } else {
    for (size_t i = 0; i < xr->values.members.count; i++) {
      struct cx_field_value
	*xv = cx_vec_get(&xr->values.members, i),
	*yv = cx_vec_get(&yr->values.members, i);
      
      if (xv->id.tag != yv->id.tag || !cx_eqval(&xv->box, &yv->box)) {
	eqval = false;
	break;
      }
    }
  }

  cx_box_init(cx_push(scope), cx->bool_type)->as_bool = eqval;
  cx_box_deinit(&x);
  cx_box_deinit(&y);
  return true;
}

static bool ok_imp(struct cx_scope *scope) {
  struct cx_box v = *cx_test(cx_pop(scope, false));
  struct cx_rec *r = v.as_ptr;
  
  cx_box_init(cx_push(scope),
	      scope->cx->bool_type)->as_bool = r->values.members.count;
  
  cx_box_deinit(&v);
  return true;
}

static bool print_imp(struct cx_scope *scope) {
  struct cx_box
    r = *cx_test(cx_pop(scope, false)),
    out = *cx_test(cx_pop(scope, false));
  cx_dump(&r, out.as_file->ptr);
  cx_box_deinit(&r);
  cx_box_deinit(&out);
  return true;
}

void cx_init_rec(struct cx *cx) {
  cx_add_macro(cx, "rec:", rec_parse); 

  cx_add_cfunc(cx, "=",
	       cx_args(cx_arg("x", cx->rec_type), cx_arg("y", cx->rec_type)),
	       cx_rets(cx_ret(cx->bool_type)),
	       eqval_imp);

  cx_add_cfunc(cx, "?",
	       cx_args(cx_arg("rec", cx->rec_type)), cx_rets(cx_ret(cx->bool_type)),
	       ok_imp);

  cx_add_cfunc(cx, "print",
	       cx_args(cx_arg("out", cx->wfile_type), cx_arg("rec", cx->rec_type)),
	       cx_rets(),
	       print_imp);

  cx_add_cfunc(cx, "get",
	       cx_args(cx_arg("rec", cx->rec_type), cx_arg("fld", cx->sym_type)),
	       cx_rets(cx_ret(cx->opt_type)),
	       get_imp);

  cx_add_cfunc(cx, "put",
	       cx_args(cx_arg("rec", cx->rec_type),
		       cx_arg("fld", cx->sym_type),
		       cx_arg("val", cx->any_type)),
	       cx_rets(),
	       put_imp);
}
