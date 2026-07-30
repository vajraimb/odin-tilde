// tb_compat.h — compatibility layer mapping the OLD TildeBackend C API
// (the tb_inst_* function-builder API that Odin's tilde glue was written
// against, ~2026-03) onto the NEW TB_GraphBuilder API in current Cuik.
//
// The old API attached nodes to a function's "current control region" and
// used explicit regions/gotos/phis. The new API builds functions through a
// TB_GraphBuilder with labels + mutable variables. Most instruction-level
// calls map 1:1; control flow is handled with the builder's label model
// (tb_builder_label_set auto-completes labels, tb_builder_br appends to
// phi-ified labels, so forward-branch-heavy code maps cleanly).
//
// NOT covered here (fixed at call sites instead):
//   - tb_pass_* / codegen driver          (tilde_proc.cpp)
//   - tb_module_create / tb_arena_create  (tilde.cpp)
//   - explicit phi construction           (3 sites in tilde_expr.cpp)
//   - loop headers with backward edges    (tilde_stmt.cpp)

#ifndef TB_COMPAT_H
#define TB_COMPAT_H

// ------------------------------------------------------------------
// current-builder registry (one function body at a time, per thread)
// ------------------------------------------------------------------
gb_global gb_thread_local TB_GraphBuilder *cg_tb_builder = nullptr;
gb_global gb_thread_local TB_Function     *cg_tb_builder_func = nullptr;
gb_global gb_thread_local TB_Node         *cg_tb_entry_syms = nullptr;

struct CgTbLabelPair { TB_Node *label, *clone; };
gb_global gb_thread_local Array<CgTbLabelPair> cg_tb_label_clones;

gb_internal void cg_tb_set_builder(TB_Function *f, TB_GraphBuilder *g) {
	cg_tb_builder = g;
	cg_tb_builder_func = g ? f : nullptr;
	cg_tb_entry_syms = g ? tb_builder_label_get(g) : nullptr;
	if (cg_tb_label_clones.allocator.proc == nullptr) {
		array_init(&cg_tb_label_clones, heap_allocator());
	}
	array_clear(&cg_tb_label_clones);
}

// NOTE(new-tb): procedure bodies can be generated re-entrantly — emitting a
// call may lazily generate a helper proc (equal/hasher procs, nested proc
// literals) whose body needs the builder itself. cg_procedure_begin used to
// clobber the single global builder and cg_procedure_end cleared it, so the
// outer body resumed with no (or a stale) builder. Keep a stack instead.
struct CgTbBuilderFrame {
	TB_GraphBuilder *builder;
	TB_Function *func;
	TB_Node *entry_syms;
	Array<CgTbLabelPair> label_clones; // header copy; backing buffer owned by the frame
};
gb_global gb_thread_local Array<CgTbBuilderFrame> cg_tb_builder_stack;

gb_internal void cg_tb_push_builder(TB_Function *f, TB_GraphBuilder *g) {
	if (cg_tb_builder_stack.allocator.proc == nullptr) {
		array_init(&cg_tb_builder_stack, heap_allocator());
	}
	CgTbBuilderFrame saved = { cg_tb_builder, cg_tb_builder_func, cg_tb_entry_syms, cg_tb_label_clones };
	array_add(&cg_tb_builder_stack, saved);

	cg_tb_builder = g;
	cg_tb_builder_func = g ? f : nullptr;
	cg_tb_entry_syms = g ? tb_builder_label_get(g) : nullptr;
	// fresh clone table for this frame so nested emission never touches the
	// outer frame's backing buffer
	array_init(&cg_tb_label_clones, heap_allocator());
}

gb_internal void cg_tb_pop_builder(void) {
	if (cg_tb_label_clones.allocator.proc != nullptr) {
		array_free(&cg_tb_label_clones);
	}
	cg_tb_label_clones = {};

	if (cg_tb_builder_stack.allocator.proc != nullptr && cg_tb_builder_stack.count > 0) {
		CgTbBuilderFrame saved = cg_tb_builder_stack.data[--cg_tb_builder_stack.count];
		cg_tb_builder = saved.builder;
		cg_tb_builder_func = saved.func;
		cg_tb_entry_syms = saved.entry_syms;
		cg_tb_label_clones = saved.label_clones;
	} else {
		cg_tb_builder = nullptr;
		cg_tb_builder_func = nullptr;
		cg_tb_entry_syms = nullptr;
	}
}

// NOTE(new-tb): a label's symbol-table slots are a mutable workspace —
// emitting code while a label is current overwrites them (stores replace
// the memory phi in slot 2). Branches therefore never target the label
// itself but an EARLY CLONE taken at creation time, which keeps pointing
// at the shared phi nodes (this is how cuik_c's own loop codegen works:
// tb_builder_loop + tb_builder_label_clone).
gb_internal TB_Node *cg_tb_branch_target(TB_Node *label) {
	for (auto const &p : cg_tb_label_clones) {
		if (p.label == label) {
			return p.clone;
		}
	}
	return label;
}


gb_internal TB_GraphBuilder *cg_tb_g(TB_Function *f) {
	if (cg_tb_builder == nullptr || cg_tb_builder_func != f) {
		gb_printf_err("tilde: cg_tb_g misuse: builder=%p builder_func=%p want_func=%p\n",
		              cast(void *)cg_tb_builder, cast(void *)cg_tb_builder_func, cast(void *)f);
		GB_ASSERT_MSG(false, "tilde: tb_inst_* used outside of an active function body builder");
	}
	return cg_tb_builder;
}

// current label is NULL after a terminator (dead code until next label_set)
gb_internal bool cg_tb_dead(TB_Function *f) {
	return tb_builder_label_get(cg_tb_g(f)) == nullptr;
}

// dead-code guard: after a terminator the builder's current label is
// NULL; old TB tolerated emitting into dead regions, so just skip.
#define TB_COMPAT_GUARD_DEAD(f) if (cg_tb_dead(f)) { return; }

// ------------------------------------------------------------------
// old type shorthands
// ------------------------------------------------------------------
// New TB only has i8/i16/i32/i64 (no arbitrary-width ints, no i128, no f16).
#define TB_TYPE_INTN(N) ((N) <= 8 ? TB_TYPE_I8 : (N) <= 16 ? TB_TYPE_I16 : (N) <= 32 ? TB_TYPE_I32 : TB_TYPE_I64)

// TODO(tilde): u128/i128 are NOT supported by new TB (no TB_TAG_I128);
// f16 has no native type either — both are kept as opaque int storage,
// matching what the old glue effectively did for f16.
#define TB_TYPE_F16_COMPAT  TB_TYPE_I16
#define TB_TYPE_I128_COMPAT TB_TYPE_I64 // layout wrong for real u128 use; LLVM backend required

// ------------------------------------------------------------------
// tb_get_data_type_size — declared in new tb.h but never implemented
// upstream; provide it here (scalar types only, which is all the glue
// ever queries).
// ------------------------------------------------------------------
inline void tb_get_data_type_size(TB_Module *mod, TB_DataType dt, size_t *size, size_t *align) {
	gb_unused(mod);
	switch (dt.type) {
	case TB_TAG_BOOL:
	case TB_TAG_I8:   *size = 1; *align = 1; break;
	case TB_TAG_I16:  *size = 2; *align = 2; break;
	case TB_TAG_I32:  *size = 4; *align = 4; break;
	case TB_TAG_F32:  *size = 4; *align = 4; break;
	case TB_TAG_I64:  *size = 8; *align = 8; break;
	case TB_TAG_F64:  *size = 8; *align = 8; break;
	case TB_TAG_PTR:  *size = 8; *align = 8; break;
	default: GB_ASSERT_MSG(0, "tb_get_data_type_size: unhandled type %d", dt.type); *size = 0; *align = 0; break;
	}
}

// ------------------------------------------------------------------
// old misc types (TB_MultiOutput/TB_SwitchEntry already exist in new tb.h)
// ------------------------------------------------------------------

// old arith-behavior enum name differs in new TB (TB_ARITHMATIC_*)
#define TB_ARITHMETIC_NONE         TB_ARITHMATIC_NONE
#define TB_ARITHMETIC_NSW          TB_ARITHMATIC_NSW
#define TB_ARITHMETIC_NUW          TB_ARITHMATIC_NUW
#define TB_ARITHMETIC_NO_TRAP_WRAP TB_ARITHMATIC_NSW

// ------------------------------------------------------------------
// constants
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_bool(TB_Function *f, bool imm)                        { return tb_builder_bool(cg_tb_g(f), imm); }
gb_internal TB_Node *tb_inst_uint(TB_Function *f, TB_DataType dt, uint64_t imm)    { return tb_builder_uint(cg_tb_g(f), dt, imm); }
gb_internal TB_Node *tb_inst_sint(TB_Function *f, TB_DataType dt, int64_t imm)     { return tb_builder_sint(cg_tb_g(f), dt, imm); }
gb_internal TB_Node *tb_inst_float32(TB_Function *f, float imm)                    { return tb_builder_float32(cg_tb_g(f), imm); }
gb_internal TB_Node *tb_inst_float64(TB_Function *f, double imm)                   { return tb_builder_float64(cg_tb_g(f), imm); }

// poison ~ arbitrary value; zero is a valid "arbitrary value"
gb_internal TB_Node *tb_inst_poison(TB_Function *f, TB_DataType dt)                { if (dt.type == TB_TAG_F32) return tb_builder_float32(cg_tb_g(f), 0); if (dt.type == TB_TAG_F64) return tb_builder_float64(cg_tb_g(f), 0); return tb_builder_uint(cg_tb_g(f), dt, 0); }

// ------------------------------------------------------------------
// casts (old arg order: f, src, dt)
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_fpxt(TB_Function *f, TB_Node *src, TB_DataType dt)               { return tb_builder_cast(cg_tb_g(f), dt, TB_FLOAT_EXT, src); }
gb_internal TB_Node *tb_inst_sxt(TB_Function *f, TB_Node *src, TB_DataType dt)                { if (src->dt.raw == dt.raw) return src; return tb_builder_cast(cg_tb_g(f), dt, TB_SIGN_EXT, src); }
gb_internal TB_Node *tb_inst_zxt(TB_Function *f, TB_Node *src, TB_DataType dt)                { if (src->dt.raw == dt.raw) return src; return tb_builder_cast(cg_tb_g(f), dt, TB_ZERO_EXT, src); }
gb_internal TB_Node *tb_inst_trunc(TB_Function *f, TB_Node *src, TB_DataType dt)              { if (src->dt.raw == dt.raw) return src; return tb_builder_cast(cg_tb_g(f), dt, TB_TRUNCATE, src); }
gb_internal TB_Node *tb_inst_fptrunc(TB_Function *f, TB_Node *src, TB_DataType dt)            { return tb_builder_cast(cg_tb_g(f), dt, TB_FLOAT_TRUNC, src); }
gb_internal TB_Node *tb_inst_int2ptr(TB_Function *f, TB_Node *src)                            { return tb_builder_cast(cg_tb_g(f), TB_TYPE_PTR, TB_BITCAST, src); }
gb_internal TB_Node *tb_inst_ptr2int(TB_Function *f, TB_Node *src, TB_DataType dt)            { return tb_builder_cast(cg_tb_g(f), dt, TB_BITCAST, src); }
gb_internal TB_Node *tb_inst_int2float(TB_Function *f, TB_Node *src, TB_DataType dt, bool is_signed) {
	return tb_builder_cast(cg_tb_g(f), dt, is_signed ? TB_INT2FLOAT : TB_UINT2FLOAT, src);
}
gb_internal TB_Node *tb_inst_float2int(TB_Function *f, TB_Node *src, TB_DataType dt, bool is_signed) {
	return tb_builder_cast(cg_tb_g(f), dt, is_signed ? TB_FLOAT2INT : TB_FLOAT2UINT, src);
}
gb_internal TB_Node *tb_inst_bitcast(TB_Function *f, TB_Node *src, TB_DataType dt)            { return tb_builder_cast(cg_tb_g(f), dt, TB_BITCAST, src); }

// ------------------------------------------------------------------
// locals & memory
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_local(TB_Function *f, TB_CharUnits size, TB_CharUnits align)     {
	// NOTE(odin-tilde): new TB asserts size > 0 && align > 0; zero-sized Odin
	// types (empty structs etc.) still need an addressable local
	if (size < 1) size = 1;
	if (align < 1) align = 1;
	return tb_builder_local(cg_tb_g(f), size, align);
}
gb_internal TB_Node *tb_inst_load(TB_Function *f, TB_DataType dt, TB_Node *addr, TB_CharUnits align, bool is_volatile) {
	if (dt.type == TB_TAG_BOOL) {
		// new TB has no bool load/store: load a byte and normalize to 0/1
		TB_Node *n = tb_builder_load(cg_tb_g(f), 0, true, TB_TYPE_I8, addr, align, is_volatile);
		TB_Node *zero = tb_builder_uint(cg_tb_g(f), TB_TYPE_I8, 0);
		return tb_builder_cmp(cg_tb_g(f), TB_CMP_NE, n, zero);
	}
	return tb_builder_load(cg_tb_g(f), 0, true, dt, addr, align, is_volatile);
}
gb_internal void tb_inst_store(TB_Function *f, TB_DataType dt, TB_Node *addr, TB_Node *val, TB_CharUnits align, bool is_volatile) {
	if (val->dt.type == TB_TAG_BOOL) {
		// new TB has no bool load/store: widen the bool to a byte first
		val = tb_builder_cast(cg_tb_g(f), TB_TYPE_I8, TB_ZERO_EXT, val);
	}
	tb_builder_store(cg_tb_g(f), 0, true, addr, val, align, is_volatile);
}
gb_internal void tb_inst_memcpy(TB_Function *f, TB_Node *dst, TB_Node *src, TB_Node *count, TB_CharUnits align) {
	tb_builder_memcpy(cg_tb_g(f), 0, true, dst, src, count, align, false);
}
gb_internal void tb_inst_memset(TB_Function *f, TB_Node *dst, TB_Node *val, TB_Node *count, TB_CharUnits align) {
	tb_builder_memset(cg_tb_g(f), 0, true, dst, val, count, align, false);
}
gb_internal void tb_inst_memzero(TB_Function *f, TB_Node *dst, TB_Node *count, TB_CharUnits align) {
	tb_builder_memzero(cg_tb_g(f), 0, true, dst, count, align, false);
}

// ------------------------------------------------------------------
// addressing
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_array_access(TB_Function *f, TB_Node *base, TB_Node *index, int64_t stride) {
	return tb_builder_ptr_array(cg_tb_g(f), base, index, stride);
}
gb_internal TB_Node *tb_inst_member_access(TB_Function *f, TB_Node *base, int64_t offset)     { return tb_builder_ptr_member(cg_tb_g(f), base, offset); }
gb_internal TB_Node *tb_inst_get_symbol_address(TB_Function *f, TB_Symbol *target)            { return tb_builder_symbol(cg_tb_g(f), target); }

// ------------------------------------------------------------------
// integer arithmetic
// ------------------------------------------------------------------
gb_internal TB_Node *cg_tb_binop_int_diag(TB_Function *f, int type, TB_Node *a, TB_Node *b, TB_ArithmeticBehavior ab) {
	// TB's integer binops do not accept TB_TAG_BOOL; Odin allows bitwise
	// ops on booleans, so widen to I8 and truncate the result back.
	if (a->dt.type == TB_TAG_BOOL && b->dt.type == TB_TAG_BOOL) {
		TB_Node *ai = tb_builder_cast(cg_tb_g(f), TB_TYPE_I8, TB_ZERO_EXT, a);
		TB_Node *bi = tb_builder_cast(cg_tb_g(f), TB_TYPE_I8, TB_ZERO_EXT, b);
		TB_Node *r  = tb_builder_binop_int(cg_tb_g(f), type, ai, bi, ab);
		return tb_builder_cast(cg_tb_g(f), TB_TYPE_BOOL, TB_TRUNCATE, r);
	}
	return tb_builder_binop_int(cg_tb_g(f), type, a, b, ab);
}
gb_internal TB_Node *tb_inst_add(TB_Function *f, TB_Node *a, TB_Node *b, TB_ArithmeticBehavior ab) { return cg_tb_binop_int_diag(f, TB_ADD, a, b, ab); }
gb_internal TB_Node *tb_inst_sub(TB_Function *f, TB_Node *a, TB_Node *b, TB_ArithmeticBehavior ab) { return tb_builder_binop_int(cg_tb_g(f), TB_SUB, a, b, ab); }
gb_internal TB_Node *tb_inst_mul(TB_Function *f, TB_Node *a, TB_Node *b, TB_ArithmeticBehavior ab) { return tb_builder_binop_int(cg_tb_g(f), TB_MUL, a, b, ab); }
gb_internal TB_Node *tb_inst_div(TB_Function *f, TB_Node *a, TB_Node *b, bool signedness)          { return tb_builder_binop_int(cg_tb_g(f), signedness ? TB_SDIV : TB_UDIV, a, b, TB_ARITHMATIC_NONE); }
gb_internal TB_Node *tb_inst_mod(TB_Function *f, TB_Node *a, TB_Node *b, bool signedness)          { return tb_builder_binop_int(cg_tb_g(f), signedness ? TB_SMOD : TB_UMOD, a, b, TB_ARITHMATIC_NONE); }
gb_internal TB_Node *tb_inst_and(TB_Function *f, TB_Node *a, TB_Node *b)                           { return cg_tb_binop_int_diag(f, TB_AND, a, b, TB_ARITHMATIC_NONE); }
gb_internal TB_Node *tb_inst_or(TB_Function *f, TB_Node *a, TB_Node *b)                            { return cg_tb_binop_int_diag(f, TB_OR, a, b, TB_ARITHMATIC_NONE); }
gb_internal TB_Node *tb_inst_xor(TB_Function *f, TB_Node *a, TB_Node *b)                           { return cg_tb_binop_int_diag(f, TB_XOR, a, b, TB_ARITHMATIC_NONE); }
gb_internal TB_Node *tb_inst_shl(TB_Function *f, TB_Node *a, TB_Node *b, TB_ArithmeticBehavior ab) { return cg_tb_binop_int_diag(f, TB_SHL, a, b, ab); }
gb_internal TB_Node *tb_inst_shr(TB_Function *f, TB_Node *a, TB_Node *b)                           { return cg_tb_binop_int_diag(f, TB_SHR, a, b, TB_ARITHMATIC_NONE); }
gb_internal TB_Node *tb_inst_sar(TB_Function *f, TB_Node *a, TB_Node *b)                           { return cg_tb_binop_int_diag(f, TB_SAR, a, b, TB_ARITHMATIC_NONE); }
gb_internal TB_Node *tb_inst_neg(TB_Function *f, TB_Node *n)                                       { return tb_builder_neg(cg_tb_g(f), n); }
gb_internal TB_Node *tb_inst_not(TB_Function *f, TB_Node *n) {
	if (n->dt.type == TB_TAG_BOOL) {
		TB_Node *ni = tb_builder_cast(cg_tb_g(f), TB_TYPE_I8, TB_ZERO_EXT, n);
		TB_Node *r  = tb_builder_not(cg_tb_g(f), ni);
		return tb_builder_cast(cg_tb_g(f), TB_TYPE_BOOL, TB_TRUNCATE, r);
	}
	return tb_builder_not(cg_tb_g(f), n);
}

// ------------------------------------------------------------------
// float arithmetic
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_fadd(TB_Function *f, TB_Node *a, TB_Node *b) { return tb_builder_binop_float(cg_tb_g(f), TB_FADD, a, b); }
gb_internal TB_Node *tb_inst_fsub(TB_Function *f, TB_Node *a, TB_Node *b) { return tb_builder_binop_float(cg_tb_g(f), TB_FSUB, a, b); }
gb_internal TB_Node *tb_inst_fmul(TB_Function *f, TB_Node *a, TB_Node *b) { return tb_builder_binop_float(cg_tb_g(f), TB_FMUL, a, b); }
gb_internal TB_Node *tb_inst_fdiv(TB_Function *f, TB_Node *a, TB_Node *b) { return tb_builder_binop_float(cg_tb_g(f), TB_FDIV, a, b); }

// ------------------------------------------------------------------
// comparisons (gt/ge are operand-swapped lt/le in new TB)
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_cmp_eq(TB_Function *f, TB_Node *a, TB_Node *b)                  { return tb_builder_cmp(cg_tb_g(f), TB_CMP_EQ, a, b); }
gb_internal TB_Node *tb_inst_cmp_ne(TB_Function *f, TB_Node *a, TB_Node *b)                  { return tb_builder_cmp(cg_tb_g(f), TB_CMP_NE, a, b); }
gb_internal TB_Node *tb_inst_cmp_ilt(TB_Function *f, TB_Node *a, TB_Node *b, bool signedness){ return tb_builder_cmp(cg_tb_g(f), signedness ? TB_CMP_SLT : TB_CMP_ULT, a, b); }
gb_internal TB_Node *tb_inst_cmp_ile(TB_Function *f, TB_Node *a, TB_Node *b, bool signedness){ return tb_builder_cmp(cg_tb_g(f), signedness ? TB_CMP_SLE : TB_CMP_ULE, a, b); }
gb_internal TB_Node *tb_inst_cmp_igt(TB_Function *f, TB_Node *a, TB_Node *b, bool signedness){ return tb_builder_cmp(cg_tb_g(f), signedness ? TB_CMP_SLT : TB_CMP_ULT, b, a); }
gb_internal TB_Node *tb_inst_cmp_ige(TB_Function *f, TB_Node *a, TB_Node *b, bool signedness){ return tb_builder_cmp(cg_tb_g(f), signedness ? TB_CMP_SLE : TB_CMP_ULE, b, a); }
gb_internal TB_Node *tb_inst_cmp_flt(TB_Function *f, TB_Node *a, TB_Node *b)                 { return tb_builder_cmp(cg_tb_g(f), TB_CMP_FLT, a, b); }
gb_internal TB_Node *tb_inst_cmp_fle(TB_Function *f, TB_Node *a, TB_Node *b)                 { return tb_builder_cmp(cg_tb_g(f), TB_CMP_FLE, a, b); }
gb_internal TB_Node *tb_inst_cmp_fgt(TB_Function *f, TB_Node *a, TB_Node *b)                 { return tb_builder_cmp(cg_tb_g(f), TB_CMP_FLT, b, a); }
gb_internal TB_Node *tb_inst_cmp_fge(TB_Function *f, TB_Node *a, TB_Node *b)                 { return tb_builder_cmp(cg_tb_g(f), TB_CMP_FLE, b, a); }

// ------------------------------------------------------------------
// unary ops & intrinsics
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_bswap(TB_Function *f, TB_Node *n)    { return tb_builder_unary(cg_tb_g(f), TB_BSWAP, n); }
gb_internal TB_Node *tb_inst_fsqrt(TB_Function *f, TB_Node *n)    { return tb_builder_unary(cg_tb_g(f), TB_FSQRT, n); }

// NOTE(odin-tilde): TB_CLZ/TB_CTZ/TB_POPCNT have no x64 isel support in the
// current Cuik/TB tree, so expand them into SWAR bit-twiddling sequences in
// plain TB IR. All three return the same data type as their input.
gb_internal int cg_tb_int_bits(TB_DataType dt) {
	switch (dt.type) {
	case TB_TAG_I8:  return 8;
	case TB_TAG_I16: return 16;
	case TB_TAG_I32: return 32;
	case TB_TAG_I64: return 64;
	}
	GB_PANIC("cg_tb_int_bits: unsupported dt %d", (int)dt.type);
	return 0;
}

gb_internal TB_Node *cg_tb_swar_popcount64(TB_Function *f, TB_Node *x) {
	TB_Node *m1 = tb_inst_uint(f, TB_TYPE_I64, 0x5555555555555555ull);
	TB_Node *m2 = tb_inst_uint(f, TB_TYPE_I64, 0x3333333333333333ull);
	TB_Node *m4 = tb_inst_uint(f, TB_TYPE_I64, 0x0f0f0f0f0f0f0f0full);
	TB_Node *h0 = tb_inst_uint(f, TB_TYPE_I64, 0x0101010101010101ull);

	TB_Node *one  = tb_inst_uint(f, TB_TYPE_I64, 1);
	TB_Node *two  = tb_inst_uint(f, TB_TYPE_I64, 2);
	TB_Node *four = tb_inst_uint(f, TB_TYPE_I64, 4);
	TB_Node *s56  = tb_inst_uint(f, TB_TYPE_I64, 56);

	x = tb_inst_sub(f, x, tb_inst_and(f, tb_inst_shr(f, x, one), m1), TB_ARITHMATIC_NONE);
	x = tb_inst_add(f,
		tb_inst_and(f, x, m2),
		tb_inst_and(f, tb_inst_shr(f, x, two), m2),
		TB_ARITHMATIC_NONE);
	x = tb_inst_and(f, tb_inst_add(f, x, tb_inst_shr(f, x, four), TB_ARITHMATIC_NONE), m4);
	x = tb_inst_shr(f, tb_inst_mul(f, x, h0, TB_ARITHMATIC_NONE), s56);
	return x;
}

gb_internal TB_Node *tb_inst_popcount(TB_Function *f, TB_Node *n) {
	TB_DataType in_dt = n->dt;
	int bits = cg_tb_int_bits(in_dt);
	TB_Node *x = n;
	if (bits < 64) {
		x = tb_inst_zxt(f, x, TB_TYPE_I64);
	}
	x = cg_tb_swar_popcount64(f, x);
	if (bits < 64) {
		x = tb_inst_trunc(f, x, in_dt);
	}
	return x;
}

// NOTE(odin-tilde): like CLZ/CTZ/POPCNT there is no x64 isel for a
// bitreverse node, so expand into the classic SWAR swap sequence plus a
// byte swap. Sub-64-bit inputs are reversed in the 64-bit domain and then
// shifted down so the reversed low bits land in place.
gb_internal TB_Node *tb_inst_reverse_bits(TB_Function *f, TB_Node *n) {
	TB_DataType in_dt = n->dt;
	int bits = cg_tb_int_bits(in_dt);
	TB_Node *x = n;
	if (bits < 64) {
		x = tb_inst_zxt(f, x, TB_TYPE_I64);
	}

	TB_Node *m1 = tb_inst_uint(f, TB_TYPE_I64, 0x5555555555555555ull);
	TB_Node *m2 = tb_inst_uint(f, TB_TYPE_I64, 0x3333333333333333ull);
	TB_Node *m4 = tb_inst_uint(f, TB_TYPE_I64, 0x0f0f0f0f0f0f0f0full);
	TB_Node *one  = tb_inst_uint(f, TB_TYPE_I64, 1);
	TB_Node *two  = tb_inst_uint(f, TB_TYPE_I64, 2);
	TB_Node *four = tb_inst_uint(f, TB_TYPE_I64, 4);

	// x = ((x >> s) & m) | ((x & m) << s) for s in 1, 2, 4
	x = tb_inst_or(f, tb_inst_and(f, tb_inst_shr(f, x, one), m1),
	               tb_inst_shl(f, tb_inst_and(f, x, m1), one, TB_ARITHMATIC_NONE));
	x = tb_inst_or(f, tb_inst_and(f, tb_inst_shr(f, x, two), m2),
	               tb_inst_shl(f, tb_inst_and(f, x, m2), two, TB_ARITHMATIC_NONE));
	x = tb_inst_or(f, tb_inst_and(f, tb_inst_shr(f, x, four), m4),
	               tb_inst_shl(f, tb_inst_and(f, x, m4), four, TB_ARITHMATIC_NONE));
	x = tb_inst_bswap(f, x);

	if (bits < 64) {
		x = tb_inst_shr(f, x, tb_inst_uint(f, TB_TYPE_I64, 64 - bits));
		x = tb_inst_trunc(f, x, in_dt);
	}
	return x;
}

gb_internal TB_Node *tb_inst_clz(TB_Function *f, TB_Node *n) {
	TB_DataType in_dt = n->dt;
	int bits = cg_tb_int_bits(in_dt);
	TB_Node *x = n;
	if (bits < 64) {
		x = tb_inst_zxt(f, x, TB_TYPE_I64);
	}
	// fill every bit below the topmost set bit, then count the set bits
	for (int s = 1; s < 64; s <<= 1) {
		x = tb_inst_or(f, x, tb_inst_shr(f, x, tb_inst_uint(f, TB_TYPE_I64, s)));
	}
	TB_Node *res = tb_inst_sub(f, tb_inst_uint(f, TB_TYPE_I64, 64), cg_tb_swar_popcount64(f, x), TB_ARITHMATIC_NONE);
	if (bits < 64) {
		res = tb_inst_sub(f, res, tb_inst_uint(f, TB_TYPE_I64, 64 - bits), TB_ARITHMATIC_NONE);
		res = tb_inst_trunc(f, res, in_dt);
	}
	return res;
}

gb_internal TB_Node *tb_inst_ctz(TB_Function *f, TB_Node *n) {
	TB_DataType in_dt = n->dt;
	int bits = cg_tb_int_bits(in_dt);
	TB_Node *x = n;
	if (bits < 64) {
		x = tb_inst_zxt(f, x, TB_TYPE_I64);
	}
	// ctz(x) = popcount(~x & (x-1)); a zero input yields the bit width
	TB_Node *v = tb_inst_and(f, tb_inst_not(f, x), tb_inst_sub(f, x, tb_inst_uint(f, TB_TYPE_I64, 1), TB_ARITHMATIC_NONE));
	if (bits < 64) {
		v = tb_inst_and(f, v, tb_inst_uint(f, TB_TYPE_I64, (~0ull) >> (64 - bits)));
	}
	x = cg_tb_swar_popcount64(f, v);
	if (bits < 64) {
		x = tb_inst_trunc(f, x, in_dt);
	}
	return x;
}

// ------------------------------------------------------------------
// select
// ------------------------------------------------------------------
gb_internal TB_Node *tb_inst_select(TB_Function *f, TB_Node *cond, TB_Node *a, TB_Node *b) { return tb_builder_select(cg_tb_g(f), cond, a, b); }

// ------------------------------------------------------------------
// calls & returns
// ------------------------------------------------------------------
gb_internal TB_MultiOutput tb_inst_call(TB_Function *f, TB_FunctionPrototype *proto, TB_Node *target, size_t param_count, TB_Node **params) {
	TB_Node **rets = tb_builder_call(cg_tb_g(f), proto, 0, target, cast(int)param_count, params);
	TB_MultiOutput out = {};
	out.count = proto->return_count;
	if (out.count == 1) {
		out.single = rets[0];
	} else if (out.count > 1) {
		out.multiple = rets;
	}
	return out;
}

gb_internal void tb_inst_ret(TB_Function *f, size_t count, TB_Node **values) {
	TB_COMPAT_GUARD_DEAD(f);
	tb_builder_ret(cg_tb_g(f), 0, cast(int)count, values);
}

// NOTE(new-tb): the entry symbol table no longer carries per-parameter
// slots (they were threaded through every label as dead phis); read the
// ROOT projections directly — they dominate the entire function.
gb_internal TB_Node *tb_inst_param(TB_Function *f, int param_id) {
	return tb_function_param(f, param_id);
}

// ------------------------------------------------------------------
// debug / misc instructions
// ------------------------------------------------------------------
gb_internal void tb_inst_location(TB_Function *f, TB_SourceFile *file, int line, int column) {
	TB_COMPAT_GUARD_DEAD(f);
	tb_builder_loc(cg_tb_g(f), 0, file, line, column);
}
gb_internal void tb_inst_set_exit_location(TB_Function *f, TB_SourceFile *file, int line, int column) {
	TB_COMPAT_GUARD_DEAD(f);
	tb_builder_loc(cg_tb_g(f), 0, file, line, column);
}
gb_internal void tb_inst_unreachable(TB_Function *f) { TB_COMPAT_GUARD_DEAD(f); tb_builder_unreachable(cg_tb_g(f), 0); }
gb_internal void tb_inst_debugbreak(TB_Function *f)  { TB_COMPAT_GUARD_DEAD(f); tb_builder_debugbreak(cg_tb_g(f), 0); }
gb_internal void tb_inst_trap(TB_Function *f)        { TB_COMPAT_GUARD_DEAD(f); tb_builder_trap(cg_tb_g(f), 0); }

// region names were cosmetic in old TB; no-op in the new builder
gb_internal void tb_inst_set_region_name(TB_Function *f, TB_Node *n, ptrdiff_t len, const char *name) {
	gb_unused(f); gb_unused(n); gb_unused(len); gb_unused(name);
}

// ------------------------------------------------------------------
// control flow
// ------------------------------------------------------------------
// NOTE(new-tb): regular labels are LAZY (cuik-style): no pre-phi, no
// clone. Phis are created on demand by tb_builder_br while the label is
// still incomplete (all forward branches arrive before the label is set
// as control). Pre-phi'ing every label like the old design explodes the
// phi count and breaks the Rogers register allocator (spill rounds
// never converge).
gb_internal TB_Node *tb_inst_region(TB_Function *f) {
	TB_GraphBuilder *g = cg_tb_g(f);
	TB_Node *templ = tb_builder_label_get(g);
	if (templ == nullptr) {
		templ = cg_tb_entry_syms;
	}
	GB_ASSERT(templ != nullptr);
	return tb_builder_label_make2(g, templ, false);
}

// Loop headers MUST be pre-phi'd: the body reads loop-carried vars
// (memory above all) before the back-edge arrives, so the phis have to
// exist up front. The clone shares the header's phis; back-edges branch
// to the clone and append to them (branching to a complete label's phi
// slots is legal, creating NEW phis there is not).
gb_internal TB_Node *tb_inst_region_loop(TB_Function *f) {
	TB_GraphBuilder *g = cg_tb_g(f);
	TB_Node *templ = tb_builder_label_get(g);
	if (templ == nullptr) {
		templ = cg_tb_entry_syms;
	}
	GB_ASSERT(templ != nullptr);
	TB_Node *label = tb_builder_label_make2(g, templ, true);
	TB_Node *clone = tb_builder_label_clone(g, label);
	array_add(&cg_tb_label_clones, CgTbLabelPair{label, clone});
	return label;
}

// tb_builder_label_set auto-completes uncompleted labels and is safe to
// call repeatedly on the same label.
gb_internal void tb_inst_set_control(TB_Function *f, TB_Node *control) {
	tb_builder_label_set(cg_tb_g(f), control);
}

gb_internal TB_Node *tb_inst_get_control(TB_Function *f) {
	return tb_builder_label_get(cg_tb_g(f));
}

gb_internal void tb_inst_goto(TB_Function *f, TB_Node *target) {
	tb_builder_br(cg_tb_g(f), cg_tb_branch_target(target));
}

// dead-code guard: after a terminator the builder's current label is
// NULL; old TB tolerated emitting into dead regions, so just skip.
// trampoline: the builder's if produces fresh completed labels; hop from
// them to the glue's own target regions.
gb_internal void tb_inst_if(TB_Function *f, TB_Node *cond, TB_Node *true_case, TB_Node *false_case) {
	TB_COMPAT_GUARD_DEAD(f);
	TB_GraphBuilder *g = cg_tb_g(f);
	TB_Node *paths[2];
	tb_builder_if(g, cond, paths);
	tb_builder_label_set(g, paths[0]);
	tb_builder_br(g, cg_tb_branch_target(true_case));
	tb_builder_label_set(g, paths[1]);
	tb_builder_br(g, cg_tb_branch_target(false_case));
}

// switch: same trampoline approach
gb_internal void tb_inst_branch(TB_Function *f, TB_DataType dt, TB_Node *key, TB_Node *default_case, size_t entry_count, const TB_SwitchEntry *keys) {
	gb_unused(dt);
	TB_COMPAT_GUARD_DEAD(f);
	TB_GraphBuilder *g = cg_tb_g(f);
	if (entry_count <= 1) {
		// the x64 multi-way-branch isel requires succ_count > 2 (i.e. at
		// least 2 cases + default), so lower tiny switches to an if-chain
		if (entry_count == 0) {
			tb_builder_br(g, cg_tb_branch_target(default_case));
			return;
		}
		TB_Node *k   = tb_builder_uint(g, key->dt, cast(uint64_t)keys[0].key);
		TB_Node *cmp = tb_builder_cmp(g, TB_CMP_EQ, key, k);
		TB_Node *paths[2];
		tb_builder_if(g, cmp, paths);
		tb_builder_label_set(g, paths[0]);
		tb_builder_br(g, cg_tb_branch_target(keys[0].value));
		tb_builder_label_set(g, paths[1]);
		tb_builder_br(g, cg_tb_branch_target(default_case));
		return;
	}
	TB_Node *br_syms = tb_builder_switch(g, key);
	TB_Node *def = tb_builder_def_case(g, br_syms, 1);
	tb_builder_label_set(g, def);
	tb_builder_br(g, cg_tb_branch_target(default_case));
	for (size_t i = 0; i < entry_count; i++) {
		TB_Node *c = tb_builder_key_case(g, br_syms, cast(uint64_t)keys[i].key, 1);
		tb_builder_label_set(g, c);
		tb_builder_br(g, cg_tb_branch_target(keys[i].value));
	}
}

#endif // TB_COMPAT_H
