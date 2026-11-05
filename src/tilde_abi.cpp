// ------------------------------------------------------------------
// System-V AMD64 small-aggregate classification for C calling-convention
// boundaries. See the declaration block in tilde.hpp for the rationale.
//
// Cuik's ABI classifier (tb/abi.c) reduces a whole aggregate to a single
// GPR blob (<= 8 bytes) or memory (> 8 bytes). SysV instead classifies
// each eightbyte of a <= 16-byte composite independently as INTEGER or
// SSE, so e.g. struct{float,float} rides one xmm register and
// struct{double,double} / complex128 ride xmm0:xmm1. Both sides of a C
// boundary (prototype construction, call-site argument emission, callee
// prologue, return emission) consult these helpers to agree on the
// transport shape.
// ------------------------------------------------------------------

gb_internal bool cg_sysv_abi_active(void) {
	if (build_context.metrics.os == TargetOs_windows) {
		return false;
	}
	return build_context.metrics.arch == TargetArch_amd64 ||
	       build_context.metrics.arch == TargetArch_arm64;
}

gb_internal bool cg_abi_is_aapcs64(void) {
	return build_context.metrics.arch == TargetArch_arm64;
}

struct cgSysVLeaf {
	i64  offset;
	i64  size;
	bool is_float;
};

gb_internal bool cg_sysv_is_float_basic(Type *t) {
	t = base_type(t);
	if (t->kind != Type_Basic) {
		return false;
	}
	switch (t->Basic.kind) {
	case Basic_f16:
	case Basic_f32:
	case Basic_f64:
		return true;
	}
	return false;
}

gb_internal void cg_sysv_collect_leaves(Type *type, i64 offset, Array<cgSysVLeaf> &leaves) {
	Type *bt = base_type(type);
	switch (bt->kind) {
	case Type_Basic:
		switch (bt->Basic.kind) {
		case Basic_complex32:
		case Basic_complex64:
		case Basic_complex128: {
			Type *et = base_complex_elem_type(bt);
			i64 es = type_size_of(et);
			bool fl = cg_sysv_is_float_basic(et);
			array_add(&leaves, {offset + 0*es, es, fl});
			array_add(&leaves, {offset + 1*es, es, fl});
			return;
		}
		case Basic_quaternion64:
		case Basic_quaternion128:
		case Basic_quaternion256: {
			Type *et = base_complex_elem_type(bt);
			i64 es = type_size_of(et);
			bool fl = cg_sysv_is_float_basic(et);
			for (i64 i = 0; i < 4; i++) {
				array_add(&leaves, {offset + i*es, es, fl});
			}
			return;
		}
		default:
			array_add(&leaves, {offset, type_size_of(bt), cg_sysv_is_float_basic(bt)});
			return;
		}

	case Type_Struct:
		if (bt->Struct.is_raw_union) {
			// conservative: unions classify INTEGER
			array_add(&leaves, {offset, type_size_of(bt), false});
			return;
		}
		type_set_offsets(bt);
		for_array(i, bt->Struct.fields) {
			Entity *f = bt->Struct.fields[i];
			cg_sysv_collect_leaves(f->type, offset + bt->Struct.offsets[i], leaves);
		}
		return;

	case Type_Array: {
		i64 es = type_size_of(bt->Array.elem);
		for (i64 i = 0; i < bt->Array.count; i++) {
			cg_sysv_collect_leaves(bt->Array.elem, offset + i*es, leaves);
		}
		return;
	}

	default:
		// pointers, slices, strings, dynamic arrays, maps, bit_sets,
		// procs, etc: integer leaves of the natural size
		array_add(&leaves, {offset, type_size_of(bt), false});
		return;
	}
}

// AAPCS64 (B.2/B.3/B.4, C.2): an HFA is 1-4 float members of one width laid
// out back to back; it rides one s/d register per member. Anything else
// <= 16 bytes rides 1-2 GPRs, larger composites are copied and passed (or
// returned, via x8) by pointer.
gb_internal cgSysVClass cg_aapcs64_classify(Type *type) {
	cgSysVClass sc = {};

	Type *bt = base_type(type);
	i64 size = type_size_of(bt);

	TEMPORARY_ALLOCATOR_GUARD();
	Array<cgSysVLeaf> leaves = {};
	leaves.allocator = temporary_allocator();
	cg_sysv_collect_leaves(bt, 0, leaves);

	bool hfa = leaves.count >= 1 && leaves.count <= 4;
	for_array(i, leaves) {
		cgSysVLeaf leaf = leaves[i];
		if (!leaf.is_float || (leaf.size != 4 && leaf.size != 8) ||
		    leaf.size != leaves[0].size || leaf.offset != i*leaf.size) {
			hfa = false;
			break;
		}
	}
	if (hfa && size == leaves.count*leaves[0].size) {
		sc.n_chunks = leaves.count;
		for_array(i, leaves) {
			sc.sse[i]    = true;
			sc.size[i]   = leaves[i].size;
			sc.offset[i] = leaves[i].offset;
		}
		return sc;
	}

	if (size > 16 || size == 0) {
		sc.n_chunks = 0; // MEMORY
		return sc;
	}
	sc.n_chunks  = size <= 8 ? 1 : 2;
	sc.size[0]   = gb_min(size, 8);
	sc.size[1]   = size - 8;
	sc.offset[1] = 8;
	return sc;
}

gb_internal cgSysVClass cg_sysv_classify(Type *type) {
	if (cg_abi_is_aapcs64()) {
		return cg_aapcs64_classify(type);
	}

	cgSysVClass sc = {};

	Type *bt = base_type(type);
	i64 size = type_size_of(bt);
	if (size > 16) {
		sc.n_chunks = 0; // MEMORY
		return sc;
	}

	TEMPORARY_ALLOCATOR_GUARD();
	Array<cgSysVLeaf> leaves = {};
	leaves.allocator = temporary_allocator();
	cg_sysv_collect_leaves(bt, 0, leaves);

	sc.n_chunks  = size <= 8 ? 1 : 2;
	sc.size[0]   = gb_min(size, 8);
	sc.size[1]   = size - 8;
	sc.offset[1] = 8;

	for (i64 k = 0; k < sc.n_chunks; k++) {
		i64 lo = k*8;
		i64 hi = lo + 8;
		bool sse = true;
		bool any = false;
		for_array(i, leaves) {
			cgSysVLeaf leaf = leaves[i];
			i64 llo = leaf.offset;
			i64 lhi = leaf.offset + gb_max(leaf.size, 1);
			if (llo >= hi || lhi <= lo) {
				continue; // no overlap with this eightbyte
			}
			any = true;
			if (llo < lo || lhi > hi) {
				// a field crossing the eightbyte boundary merges the
				// classes conservatively to INTEGER on both sides
				sse = false;
			}
			if (!leaf.is_float) {
				sse = false;
			}
		}
		// pure-padding chunks (no overlapping leaf) stay INTEGER
		sc.sse[k] = any && sse;
	}
	return sc;
}

gb_internal bool cg_sysv_needs_split(Type *type) {
	if (is_type_float(type)) {
		return false; // plain f16/f32/f64 already travel as scalars
	}
	if (cg_abi_is_aapcs64() && is_type_simd_vector(type)) {
		return false; // short vectors keep the existing struct-of-lanes path
	}
	cgSysVClass sc = cg_sysv_classify(type);
	if (sc.n_chunks >= 2) {
		return true;
	}
	return sc.n_chunks == 1 && sc.sse[0];
}

gb_internal TB_DataType cg_sysv_chunk_dt(cgSysVClass const &sc, int k) {
	if (sc.sse[k]) {
		return sc.size[k] <= 4 ? TB_TYPE_F32 : TB_TYPE_F64;
	}
	return cg_gpr_blob_dt(sc.size[k]);
}

gb_internal TB_DebugType *cg_sysv_chunk_debug_type(cgModule *m, cgSysVClass const &sc, int k) {
	if (sc.sse[k]) {
		return sc.size[k] <= 4 ? tb_debug_get_float32(m->mod) : tb_debug_get_float64(m->mod);
	}
	i64 cap = cg_gpr_blob_capacity(sc.size[k]);
	return tb_debug_get_integer(m->mod, false, cast(int)(cap*8));
}
