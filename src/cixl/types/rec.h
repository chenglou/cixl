#ifndef CX_REC_H
#define CX_REC_H

#include "cixl/box.h"
#include "cixl/set.h"
#include "cixl/type.h"
#include "cixl/types/sym.h"

struct cx;

struct cx_rec_type {
  struct cx_type imp;
  struct cx_set fields;
};

struct cx_field {
  struct cx_sym id;
  struct cx_type *type;
};

struct cx_rec_type *cx_rec_type_new(struct cx *cx, const char *id);

struct cx_rec_type *cx_rec_type_init(struct cx_rec_type *type,
				     struct cx *cx,
				     const char *id);

struct cx_rec_type *cx_rec_type_reinit(struct cx_rec_type *type);
struct cx_rec_type *cx_rec_type_deinit(struct cx_rec_type *type);

void cx_derive_rec(struct cx_rec_type *child, struct cx_type *parent);

bool cx_add_field(struct cx_rec_type *type,
		  struct cx_sym fid,
		  struct cx_type *ftype,
		  bool silent);

struct cx_rec {
  struct cx_rec_type *type;
  struct cx_set values;
  unsigned int nrefs;
};

struct cx_field_value {
  struct cx_sym id;
  struct cx_box box;
}; 

struct cx_rec *cx_rec_new(struct cx_rec_type *type);
struct cx_rec *cx_rec_ref(struct cx_rec *rec);
void cx_rec_deref(struct cx_rec *rec);

struct cx_box *cx_rec_get(struct cx_rec *rec, struct cx_sym fid);
void cx_rec_put(struct cx_rec *rec, struct cx_sym fid, struct cx_box *v);

#endif
