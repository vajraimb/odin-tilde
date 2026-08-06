gb_internal cgValue cg_flatten_value(cgProcedure *p, cgValue value) {
	GB_ASSERT(value.kind != cgValue_Multi);
	if (value.kind == cgValue_Symbol) {
		GB_ASSERT(is_type_internally_pointer_like(value.type));
		return cg_value(tb_inst_get_symbol_address(p->func, value.symbol), value.type);
	} else if (value.kind == cgValue_Addr) {
		// TODO(bill): Is this a good idea?
		// this converts an lvalue to an rvalue if trivially possible
		TB_DataType dt = cg_data_type(value.type);
		if (!TB_IS_VOID_TYPE(dt)) {
			TB_CharUnits align = cast(TB_CharUnits)type_align_of(value.type);
			return cg_value(tb_inst_load(p->func, dt, value.node, align, false), value.type);
		}
	}
	return value;
}

gb_internal cgValue cg_emit_select(cgProcedure *p, cgValue const &cond, cgValue const &x, cgValue const &y) {
	GB_ASSERT(x.kind == y.kind);
	GB_ASSERT(cond.kind == cgValue_Value);
	cgValue res = x;
	res.node = tb_inst_select(p->func, cond.node, x.node, y.node);
	return res;
}


gb_internal bool cg_is_expr_untyped_const(Ast *expr) {
	auto const &tv = type_and_value_of_expr(expr);
	if (is_type_untyped(tv.type)) {
		return tv.value.kind != ExactValue_Invalid;
	}
	return false;
}
gb_internal cgValue cg_expr_untyped_const_to_typed(cgProcedure *p, Ast *expr, Type *t) {
	GB_ASSERT(is_type_typed(t));
	auto const &tv = type_and_value_of_expr(expr);
	return cg_const_value(p, t, tv.value);
}

gb_internal cgContextData *cg_push_context_onto_stack(cgProcedure *p, cgAddr ctx) {
	ctx.kind = cgAddr_Context;
	cgContextData *cd = array_add_and_get(&p->context_stack);
	cd->ctx = ctx;
	cd->scope_index = p->scope_index;
	return cd;
}

gb_internal cgAddr cg_find_or_generate_context_ptr(cgProcedure *p) {
	if (p->context_stack.count > 0) {
		return p->context_stack[p->context_stack.count-1].ctx;
	}

	Type *pt = base_type(p->type);
	GB_ASSERT(pt->kind == Type_Proc);
	GB_ASSERT(pt->Proc.calling_convention != ProcCC_Odin);

	cgAddr c = cg_add_local(p, t_context, nullptr, true);
	tb_function_attrib_variable(p->func, c.addr.node, nullptr, -1, "context", cg_debug_type(p->module, t_context));
	c.kind = cgAddr_Context;
	// lb_emit_init_context(p, c);
	cg_push_context_onto_stack(p, c);
	// lb_add_debug_context_variable(p, c);

	return c;
}

gb_internal cgValue cg_find_value_from_entity(cgModule *m, Entity *e) {
	e = strip_entity_wrapping(e);
	GB_ASSERT(e != nullptr);

	GB_ASSERT(e->token.string != "_");

	if (e->kind == Entity_Procedure) {
		return cg_find_procedure_value_from_entity(m, e);
	}

	cgValue *found = nullptr;
	rw_mutex_shared_lock(&m->values_mutex);
	found = map_get(&m->values, e);
	rw_mutex_shared_unlock(&m->values_mutex);
	if (found) {
		return *found;
	}

	GB_PANIC("\n\tError in: %s, missing value '%.*s'\n", token_pos_to_string(e->token.pos), LIT(e->token.string));
	return {};
}

gb_internal cgValue cg_get_using_variable(cgProcedure *p, Entity *e) {
	GB_ASSERT(e->kind == Entity_Variable && e->flags & EntityFlag_Using);
	InternedString name = entity_interned_name(e);
	Entity *parent = e->using_parent;
	Selection sel = lookup_field(parent->type, name, false);
	GB_ASSERT(sel.entity != nullptr);
	cgValue *pv = map_get(&p->module->values, parent);

	cgValue v = {};

	if (pv == nullptr && parent->flags & EntityFlag_SoaPtrField) {
		// NOTE(bill): using SOA value (probably from for-in statement)
		GB_PANIC("TODO(bill): cg_get_soa_variable_addr");
		// cgAddr parent_addr = cg_get_soa_variable_addr(p, parent);
		// v = cg_addr_get_ptr(p, parent_addr);
	} else if (pv != nullptr) {
		v = *pv;
	} else {
		GB_ASSERT_MSG(e->using_expr != nullptr, "%.*s %.*s", LIT(e->token.string), LIT(name.string()));
		v = cg_build_addr_ptr(p, e->using_expr);
	}
	GB_ASSERT(v.node != nullptr);
	GB_ASSERT_MSG(parent->type == type_deref(v.type), "%s %s", type_to_string(parent->type), type_to_string(v.type));
	cgValue ptr = cg_emit_deep_field_gep(p, v, sel);
	// if (parent->scope) {
	// 	if ((parent->scope->flags & (ScopeFlag_File|ScopeFlag_Pkg)) == 0) {
	// 		cg_add_debug_local_variable(p, ptr.value, e->type, e->token);
	// 	}
	// } else {
	// 	cg_add_debug_local_variable(p, ptr.value, e->type, e->token);
	// }
	return ptr;
}
// NOTE(odin-tilde): pthread-key TLS shim. Cuik does not implement Mach-O TLV
// access sequences (its TLS support is Windows-centric: x64 is_tls_symbol is
// defined but never called, and there is no TLV relocation emission), so
// @(thread_local) variables are lowered to offsets into a fixed-size
// per-thread table fetched through a pthread key (runtime __tilde_tls_base,
// base/runtime/tilde_tls_darwin.odin). The table is calloc-zeroed per thread;
// non-zero initializers are rejected at compile time (see
// cg_check_thread_local_initializer).

gb_internal bool cg_entity_is_thread_local(Entity *e) {
	return e != nullptr && e->kind == Entity_Variable && e->Variable.thread_local_model != "";
}

gb_internal i64 cg_tls_offset_for(cgModule *m, Entity *e) {
	mutex_lock(&m->tls_mutex);
	i64 *found = map_get(&m->tls_offsets, e);
	if (found) {
		mutex_unlock(&m->tls_mutex);
		return *found;
	}
	i64 align = gb_max(type_align_of(e->type), 1);
	i64 off = (m->tls_size + align - 1) & ~(align - 1);
	m->tls_size = off + type_size_of(e->type);
	GB_ASSERT_MSG(m->tls_size <= CG_TLS_TABLE_SIZE,
		"tilde: @(thread_local) storage exceeds the %d-byte per-thread table", CG_TLS_TABLE_SIZE);
	map_set(&m->tls_offsets, e, off);
	mutex_unlock(&m->tls_mutex);
	return off;
}

gb_internal cgValue cg_tls_address(cgProcedure *p, Entity *e) {
	cg_tls_ensure_runtime_globals(p->module);
	i64 offset = cg_tls_offset_for(p->module, e);
	auto args = slice_make<cgValue>(temporary_allocator(), 1);
	args[0] = cg_const_int(p, t_int, CG_TLS_TABLE_SIZE);
	cgValue base = cg_emit_runtime_call(p, "__tilde_tls_base", args);
	GB_ASSERT(base.node != nullptr);
	TB_Node *node = base.node;
	if (offset != 0) {
		node = tb_inst_member_access(p->func, node, offset);
	}
	return cg_value(node, alloc_type_pointer(e->type));
}

// NOTE(odin-tilde): __tilde_tls_base's state globals (_tilde_tls_key/_state)
// are referenced only by synthesized calls, so the checker's dependency graph
// never marks them and cg_global_variables_create skips them. Create their TB
// globals lazily on first TLS access.
gb_internal void cg_tls_ensure_runtime_globals(cgModule *m) {
	static char const *names[] = { "_tilde_tls_key", "_tilde_tls_state" };
	AstPackage *pkg = m->info->runtime_package;
	for (char const *name : names) {
		u32 hash = 0;
		InternedString interned = string_interner_insert(make_string_c(name), 0, &hash);
		Entity *e = scope_lookup_current(pkg->scope, interned, hash);
		if (e == nullptr) {
			continue; // shim 文件未参与构建（非 darwin）
		}
		rw_mutex_shared_lock(&m->values_mutex);
		bool exists = map_get(&m->values, e) != nullptr;
		rw_mutex_shared_unlock(&m->values_mutex);
		if (exists) {
			continue;
		}

		mutex_lock(&m->tls_mutex);
		rw_mutex_shared_lock(&m->values_mutex);
		exists = map_get(&m->values, e) != nullptr;
		rw_mutex_shared_unlock(&m->values_mutex);
		if (!exists) {
			String gname = cg_get_entity_name(m, e);
			TB_DebugType *debug_type = cg_debug_type(m, e->type);
			TB_Global *g = tb_global_create(m->mod, gname.len, cast(char const *)gname.text, debug_type, TB_LINKAGE_PRIVATE);
			tb_global_set_storage(m->mod, tb_module_get_data(m->mod), g, type_size_of(e->type), type_align_of(e->type), 0);
			cgValue gv = cg_value(g, alloc_type_pointer(e->type));
			cg_add_symbol(m, e, cast(TB_Symbol *)g);
			cg_add_entity(m, e, gv);
			cg_add_member(m, gname, gv);
		}
		mutex_unlock(&m->tls_mutex);
	}
}

gb_internal bool cg_exact_value_is_zero(ExactValue const &v) {
	switch (v.kind) {
	case ExactValue_Invalid:   return true;  // 无显式初始化
	case ExactValue_Bool:      return !v.value_bool;
	case ExactValue_Integer:   return big_int_is_zero(&v.value_integer);
	case ExactValue_Float:     return v.value_float == 0;
	case ExactValue_Pointer:   return v.value_pointer == 0;
	case ExactValue_Typeid:    return v.value_typeid == nullptr;
	case ExactValue_String:    return v.value_string.len == 0;
	case ExactValue_String16:  return v.value_string16.len == 0;
	case ExactValue_Complex:   return v.value_complex == nullptr ||
		(v.value_complex->real == 0 && v.value_complex->imag == 0);
	case ExactValue_Quaternion: return v.value_quaternion == nullptr ||
		(v.value_quaternion->real == 0 && v.value_quaternion->imag == 0 &&
		 v.value_quaternion->jmag == 0 && v.value_quaternion->kmag == 0);
	case ExactValue_Compound:
	case ExactValue_Procedure:
		return false; // 保守：复合/过程初始化一律按非零处理
	}
	return false;
}

gb_internal void cg_check_thread_local_initializer(Entity *e, ExactValue const *constant_value) {
	if (!cg_entity_is_thread_local(e)) {
		return;
	}
	if (constant_value != nullptr && cg_exact_value_is_zero(*constant_value)) {
		return; // 零初始化：每线程表本来就是 calloc 清零的
	}
	GB_PANIC("tilde: @(thread_local) variable '%.*s' has a %s initializer; "
		"only zero-initialized thread-locals are supported for now",
		LIT(e->token.string), constant_value ? "non-zero constant" : "runtime (non-constant)");
}

gb_internal cgAddr cg_build_addr_from_entity(cgProcedure *p, Entity *e, Ast *expr) {
	GB_ASSERT(e != nullptr);
	if (e->kind == Entity_Constant) {
		Type *t = default_type(type_of_expr(expr));
		// NOTE(odin-tilde): address of a constant — materialize the
		// ExactValue into a generated global (mirrors
		// lb_add_global_generated_from_procedure; the value is written
		// directly from the ExactValue since scalar TB nodes cannot be
		// global initializers).
		cgModule *m = p->module;
		String name = cg_generated_name(m, p, "ggv");
		TB_Global *g = tb_global_create(m->mod, name.len, cast(char const *)name.text, cg_debug_type(m, t), TB_LINKAGE_PRIVATE);
		i64 max_objects = cg_global_const_calculate_region_count(e->Constant.value, t);
		tb_global_set_storage(m->mod, tb_module_get_data(m->mod), g, type_size_of(t), type_align_of(t), max_objects);
		cg_global_const_add_region(m, e->Constant.value, t, g, 0);
		TB_Node *node = tb_inst_get_symbol_address(p->func, cast(TB_Symbol *)g);
		return cg_addr(cg_value(node, alloc_type_pointer(t)));
	}

	if (cg_entity_is_thread_local(e)) {
		// NOTE(odin-tilde): Cuik lacks Mach-O TLV sequences; thread-locals
		// are lowered to offsets into a per-thread table fetched via a
		// pthread key (runtime __tilde_tls_base). See KNOWN_ISSUES #3.
		return cg_addr(cg_tls_address(p, e));
	}

	cgAddr *local_found = map_get(&p->variable_map, e);
	if (local_found) {
		return *local_found;
	}

	cgValue v = {};

	cgModule *m = p->module;

	rw_mutex_lock(&m->values_mutex);
	cgValue *found = map_get(&m->values, e);
	rw_mutex_unlock(&m->values_mutex);
	if (found) {
		v = *found;
	} else if (e->kind == Entity_Variable && e->flags & EntityFlag_Using) {
		// NOTE(bill): Calculate the using variable every time
		v = cg_get_using_variable(p, e);
	} else if (e->flags & EntityFlag_SoaPtrField) {
		return map_must_get(&p->soa_values_map, e);
	}


	if (v.node == nullptr) {
		cgValue v = cg_find_value_from_entity(m, e);
		v = cg_flatten_value(p, v);
		return cg_addr(v);
	}

	return cg_addr(v);
}

gb_internal cgValue cg_emit_union_tag_ptr(cgProcedure *p, cgValue const &parent_ptr) {
	Type *t = parent_ptr.type;
	Type *ut = base_type(type_deref(t));
	GB_ASSERT_MSG(is_type_pointer(t), "%s", type_to_string(t));
	GB_ASSERT_MSG(ut->kind == Type_Union, "%s", type_to_string(t));

	GB_ASSERT(!is_type_union_maybe_pointer_original_alignment(ut));
	GB_ASSERT(!is_type_union_maybe_pointer(ut));
	GB_ASSERT(type_size_of(ut) > 0);

	Type *tag_type = union_tag_type(ut);
	i64 tag_offset = ut->Union.variant_block_size;

	GB_ASSERT(parent_ptr.kind == cgValue_Value);
	TB_Node *ptr = parent_ptr.node;
	TB_Node *tag_ptr = tb_inst_member_access(p->func, ptr, tag_offset);
	return cg_value(tag_ptr, alloc_type_pointer(tag_type));
}

// PATCH(odin-tilde): port of lb_emit_store_union_variant (simplified)
gb_internal void cg_emit_store_union_variant(cgProcedure *p, cgValue parent_ptr, cgValue value, Type *variant_type) {
	Type *pt = base_type(type_deref(parent_ptr.type));
	GB_ASSERT(pt->kind == Type_Union);
	GB_ASSERT_MSG(union_is_variant_of(pt, variant_type), "%s %s", type_to_string(pt), type_to_string(variant_type));

	if (parent_ptr.kind == cgValue_Symbol) {
		parent_ptr = cg_value(tb_inst_get_symbol_address(p->func, parent_ptr.symbol), parent_ptr.type);
	} else if (parent_ptr.kind == cgValue_Addr) {
		parent_ptr = cg_emit_load(p, parent_ptr);
	}
	// Variant payload lives at offset 0 of the union; reinterpret the pointer
	// as pointer-to-variant so cg_emit_store's type check holds.
	cgValue variant_ptr = cg_value(parent_ptr.node, alloc_type_pointer(variant_type));

	if (is_type_union_maybe_pointer(pt) || type_size_of(pt) == 0) {
		// Representation is just the (possibly nil) pointer, no tag.
		// NOTE: store the value directly; converting to the union type would
		// recurse right back into this function.
		cg_emit_store(p, variant_ptr, value);
		return;
	}

	if (pt->Union.kind == UnionType_shared_nil) {
		// All variants share a nil value; a nil variant means a nil union.
		// NOTE(odin-tilde): proper nil-branching, ported from
		// lb_emit_store_union_variant — without it a zero-valued variant
		// (e.g. Allocator_Error.None) got its real tag instead of collapsing
		// to a nil union, breaking `err != nil` / or_return checks.
		GB_ASSERT(type_size_of(variant_type));

		TB_Node *if_nil     = cg_control_region(p, "shared_nil_if_nil");
		TB_Node *if_not_nil = cg_control_region(p, "shared_nil_if_not_nil");
		TB_Node *done       = cg_control_region(p, "shared_nil_done");

		cgValue cond_is_nil = cg_emit_comp_against_nil(p, Token_CmpEq, value);
		tb_inst_if(p->func, cond_is_nil.node, if_nil, if_not_nil);

		tb_inst_set_control(p->func, if_nil);
		cg_emit_store(p, parent_ptr, cg_const_nil(p, type_deref(parent_ptr.type)));
		tb_inst_goto(p->func, done);

		tb_inst_set_control(p->func, if_not_nil);
		cg_emit_store(p, variant_ptr, value);
		cg_emit_store(p, cg_emit_union_tag_ptr(p, parent_ptr), cg_const_union_tag(p, pt, variant_type));
		tb_inst_goto(p->func, done);

		tb_inst_set_control(p->func, done);
		return;
	}

	if (type_size_of(variant_type) != 0) {
		cg_emit_store(p, variant_ptr, value);
	}
	cg_emit_store(p, cg_emit_union_tag_ptr(p, parent_ptr), cg_const_union_tag(p, pt, variant_type));
}



gb_internal cgValue cg_emit_byte_swap(cgProcedure *p, cgValue value, Type *end_type);

gb_internal cgValue cg_correct_endianness(cgProcedure *p, cgValue value) {
	Type *src = core_type(value.type);
	GB_ASSERT(is_type_integer(src) || is_type_float(src));
	if (is_type_different_to_arch_endianness(src)) {
		// NOTE(odin-tilde): mirrors lb_correct_endianness — normalize a
		// foreign-endian scalar (e.g. u32be slice indices in timezone
		// parsing) to the platform type before use.
		Type *platform_src_type = integer_endian_type_to_platform_type(src);
		value = cg_emit_byte_swap(p, value, platform_src_type);
	}
	return value;
}

gb_internal cgValue cg_emit_transmute(cgProcedure *p, cgValue value, Type *type) {
	GB_ASSERT(type_size_of(value.type) == type_size_of(type));

	value = cg_flatten_value(p, value);

	if (are_types_identical(value.type, type)) {
		return value;
	}
	if (are_types_identical(core_type(value.type), core_type(type))) {
		value.type = type;
		return value;
	}

	i64 src_align = type_align_of(value.type);
	i64 dst_align = type_align_of(type);

	if (dst_align > src_align) {
		cgAddr local = cg_add_local(p, type, nullptr, false);
		cgValue dst = local.addr;
		dst.type = alloc_type_pointer(value.type);
		cg_emit_store(p, dst, value);
		return cg_addr_load(p, local);
	}

	TB_DataType dt = cg_data_type(type);
	if (TB_IS_VOID_TYPE(dt)) {
		// NOTE(bill): aggregate destination, transmute through memory
		cgAddr local = cg_add_local(p, type, nullptr, false);
		cgValue dst = local.addr;
		dst.type = alloc_type_pointer(value.type);
		cg_emit_store(p, dst, value);
		return cg_addr_load(p, local);
	}
	switch (value.kind) {
	case cgValue_Value:
		GB_ASSERT_MSG(!TB_IS_VOID_TYPE(dt), "%d %s -> %s", dt.type, type_to_string(value.type), type_to_string(type));
		value.type = type;
		if (value.node->dt.raw != dt.raw) {
			switch (value.node->dt.type) {
			case TB_TAG_I8:
			case TB_TAG_I16:
			case TB_TAG_I32:
			case TB_TAG_I64:
				switch (dt.type) {
				case TB_TAG_I8:
				case TB_TAG_I16:
				case TB_TAG_I32:
				case TB_TAG_I64:
					break;
				case TB_TAG_F32:
				case TB_TAG_F64:
					value.node = tb_inst_bitcast(p->func, value.node, dt);
					break;
				case TB_TAG_PTR:
					value.node = tb_inst_int2ptr(p->func, value.node);
					break;
				}
				break;
			case TB_TAG_F32:
			case TB_TAG_F64:
				switch (dt.type) {
				case TB_TAG_I8:
				case TB_TAG_I16:
				case TB_TAG_I32:
				case TB_TAG_I64:
					value.node = tb_inst_bitcast(p->func, value.node, dt);
					break;
				case TB_TAG_F32:
				case TB_TAG_F64:
					break;
				case TB_TAG_PTR:
					value.node = tb_inst_bitcast(p->func, value.node, TB_TYPE_INTPTR);
					value.node = tb_inst_int2ptr(p->func, value.node);
					break;
				}
				break;
			case TB_TAG_PTR:
				switch (dt.type) {
				case TB_TAG_I8:
				case TB_TAG_I16:
				case TB_TAG_I32:
				case TB_TAG_I64:
					value.node = tb_inst_ptr2int(p->func, value.node, dt);
					break;
				case TB_TAG_F32:
				case TB_TAG_F64:
					value.node = tb_inst_ptr2int(p->func, value.node, TB_TYPE_INTPTR);
					value.node = tb_inst_bitcast(p->func, value.node, dt);
					break;
				case TB_TAG_PTR:
					break;
				}
				break;
			}
		}
		return value;
	case cgValue_Addr:
		value.type = type;
		return value;
	case cgValue_Symbol:
		GB_PANIC("should be handled above");
		break;
	case cgValue_Multi:
		GB_PANIC("cannot transmute multiple values at once");
		break;
	}
	return value;

}
gb_internal cgValue cg_emit_byte_swap(cgProcedure *p, cgValue value, Type *end_type) {
	GB_ASSERT(type_size_of(value.type) == type_size_of(end_type));

	if (type_size_of(value.type) < 2) {
		return value;
	}

	if (is_type_float(value.type)) {
		i64 sz = type_size_of(value.type);
		Type *integer_type = nullptr;
		switch (sz) {
		case 2: integer_type = t_u16; break;
		case 4: integer_type = t_u32; break;
		case 8: integer_type = t_u64; break;
		}
		GB_ASSERT(integer_type != nullptr);
		value = cg_emit_transmute(p, value, integer_type);
	}

	GB_ASSERT(value.kind == cgValue_Value);

	value.node = tb_inst_bswap(p->func, value.node);
	return cg_emit_transmute(p, value, end_type);
}

// NOTE(odin-tilde): TB has no 128-bit integer type, so u128/i128 (and their
// endian variants) are opaque 16-byte aggregates (cg_data_type returns VOID).
// Everything below lowers operations on them to pairs of u64 limbs in memory.
// Division/modulus go through the runtime's soft implementations (udivti3 &
// co in base/runtime/internal_i128.odin), which are themselves compiled by
// this backend on top of these primitives.

gb_internal bool cg_is_type_integer_128(Type *t) {
	t = core_type(t);
	return is_type_integer(t) && type_size_of(t) == 16;
}

gb_internal bool cg_u128_is_swapped(Type *t) {
	// big-endian variants on a little-endian target store the value
	// byte-swapped: the low limb lives at offset 8, both limbs bswap'd
	return is_type_different_to_arch_endianness(t);
}

gb_internal TB_Node *cg_emit_u128_limb_load(cgProcedure *p, TB_Node *base_ptr, bool swapped, int limb) {
	GB_ASSERT(limb == 0 || limb == 1);
	i64 off = 8 * (swapped ? (1 - limb) : limb);
	TB_Node *ptr = base_ptr;
	if (off != 0) {
		ptr = tb_inst_member_access(p->func, base_ptr, off);
	}
	TB_Node *n = tb_inst_load(p->func, TB_TYPE_I64, ptr, 8, false);
	if (swapped) {
		n = tb_inst_bswap(p->func, n);
	}
	return n;
}

gb_internal void cg_emit_u128_limb_store(cgProcedure *p, TB_Node *base_ptr, bool swapped, int limb, TB_Node *n) {
	GB_ASSERT(limb == 0 || limb == 1);
	i64 off = 8 * (swapped ? (1 - limb) : limb);
	TB_Node *ptr = base_ptr;
	if (off != 0) {
		ptr = tb_inst_member_access(p->func, base_ptr, off);
	}
	if (swapped) {
		n = tb_inst_bswap(p->func, n);
	}
	tb_inst_store(p->func, TB_TYPE_I64, ptr, n, 8, false);
}

gb_internal TB_Node *cg_u128_addr_of(cgProcedure *p, cgValue v) {
	cgValue a = cg_address_from_load_or_generate_local(p, v);
	GB_ASSERT(a.node->dt.type == TB_TAG_PTR);
	return a.node;
}

gb_internal cgValue cg_u128_make_value(cgProcedure *p, Type *type, bool swapped, TB_Node *lo, TB_Node *hi) {
	cgAddr res = cg_add_local(p, type, nullptr, false);
	cg_emit_u128_limb_store(p, res.addr.node, swapped, 0, lo);
	cg_emit_u128_limb_store(p, res.addr.node, swapped, 1, hi);
	return cg_addr_load(p, res);
}

gb_internal cgValue cg_emit_arith_u128(cgProcedure *p, TokenKind op, cgValue lhs, cgValue rhs, Type *type) {
	lhs = cg_emit_conv(p, lhs, type);
	rhs = cg_emit_conv(p, rhs, type);

	TB_Function *f = p->func;
	bool swapped = cg_u128_is_swapped(type);
	bool is_signed = !is_type_unsigned(type);
	TB_ArithmeticBehavior ab = cast(TB_ArithmeticBehavior)0;

	if (op == Token_Quo || op == Token_Mod || op == Token_ModMod) {
		// soft division through the runtime (udivmod128 lowers to u64 limbs)
		Type *rt = is_signed ? t_i128 : t_u128;
		char const *name = nullptr;
		if (is_signed) {
			name = (op == Token_Quo) ? "divti3" : "modti3";
		} else {
			name = (op == Token_Quo) ? "udivti3" : "umodti3";
		}
		auto args = slice_make<cgValue>(permanent_allocator(), 2);
		args[0] = cg_emit_conv(p, lhs, rt);
		args[1] = cg_emit_conv(p, rhs, rt);
		cgValue res = cg_emit_runtime_call(p, name, args);
		res = cg_emit_conv(p, res, type);
		if (op == Token_ModMod && is_signed) {
			// ((x %% y) + y) %% y
			res = cg_emit_arith_u128(p, Token_Add, res, rhs, type);
			res = cg_emit_arith_u128(p, Token_Mod, res, rhs, type);
		}
		return res;
	}

	TB_Node *lp = cg_u128_addr_of(p, lhs);
	TB_Node *rp = cg_u128_addr_of(p, rhs);
	TB_Node *a0 = cg_emit_u128_limb_load(p, lp, swapped, 0);
	TB_Node *a1 = cg_emit_u128_limb_load(p, lp, swapped, 1);
	TB_Node *b0 = cg_emit_u128_limb_load(p, rp, swapped, 0);
	TB_Node *b1 = cg_emit_u128_limb_load(p, rp, swapped, 1);

	TB_Node *r0 = nullptr;
	TB_Node *r1 = nullptr;

	switch (op) {
	case Token_Add: {
		r0 = tb_inst_add(f, a0, b0, ab);
		TB_Node *carry = tb_inst_cmp_ilt(f, r0, a0, false);
		carry = tb_inst_zxt(f, carry, TB_TYPE_I64);
		r1 = tb_inst_add(f, tb_inst_add(f, a1, b1, ab), carry, ab);
		break;
	}
	case Token_Sub: {
		TB_Node *borrow = tb_inst_cmp_ilt(f, a0, b0, false);
		borrow = tb_inst_zxt(f, borrow, TB_TYPE_I64);
		r0 = tb_inst_sub(f, a0, b0, ab);
		r1 = tb_inst_sub(f, tb_inst_sub(f, a1, b1, ab), borrow, ab);
		break;
	}
	case Token_Mul: {
		// low 128 bits of the product: a0*b0 in full, plus the low words
		// of the cross terms
		GB_ASSERT(cg_tb_builder != nullptr);
		TB_Node *hi0 = nullptr;
		TB_Node *lo0 = tb_builder_umulpair(cg_tb_g(f), a0, b0, &hi0);
		TB_Node *c1 = tb_inst_mul(f, a0, b1, ab);
		TB_Node *c2 = tb_inst_mul(f, a1, b0, ab);
		r0 = lo0;
		r1 = tb_inst_add(f, tb_inst_add(f, hi0, c1, ab), c2, ab);
		break;
	}
	case Token_And:
		r0 = tb_inst_and(f, a0, b0);
		r1 = tb_inst_and(f, a1, b1);
		break;
	case Token_Or:
		r0 = tb_inst_or(f, a0, b0);
		r1 = tb_inst_or(f, a1, b1);
		break;
	case Token_Xor:
		r0 = tb_inst_xor(f, a0, b0);
		r1 = tb_inst_xor(f, a1, b1);
		break;
	case Token_AndNot:
		r0 = tb_inst_and(f, a0, tb_inst_not(f, b0));
		r1 = tb_inst_and(f, a1, tb_inst_not(f, b1));
		break;

	case Token_Shl:
	case Token_Shr: {
		cgValue bits_v = cg_flatten_value(p, cg_emit_conv(p, rhs, t_u64));
		TB_Node *bits = bits_v.node;
		TB_Node *zero  = tb_inst_uint(f, TB_TYPE_I64, 0);
		TB_Node *is0   = tb_inst_cmp_eq(f, bits, zero);
		TB_Node *lt64  = tb_inst_cmp_ilt(f, bits, tb_inst_uint(f, TB_TYPE_I64, 64), false);
		TB_Node *ge128 = tb_inst_cmp_ige(f, bits, tb_inst_uint(f, TB_TYPE_I64, 128), false);
		TB_Node *bits_m64 = tb_inst_sub(f, bits, tb_inst_uint(f, TB_TYPE_I64, 64), ab);
		TB_Node *inv      = tb_inst_sub(f, tb_inst_uint(f, TB_TYPE_I64, 64), bits, ab);

		bool shr = (op == Token_Shr);
		bool arith = shr && is_signed;
		TB_Node *sign = arith ? tb_inst_sar(f, a1, tb_inst_uint(f, TB_TYPE_I64, 63)) : zero;

		// path A: 0 < bits < 64
		TB_Node *hiA, *loA;
		if (!shr) {
			hiA = tb_inst_or(f, tb_inst_shl(f, a1, bits, ab), tb_inst_shr(f, a0, inv));
			loA = tb_inst_shl(f, a0, bits, ab);
		} else {
			loA = tb_inst_or(f, tb_inst_shr(f, a0, bits), tb_inst_shl(f, a1, inv, ab));
			hiA = arith ? tb_inst_sar(f, a1, bits) : tb_inst_shr(f, a1, bits);
		}
		// path B: 64 <= bits < 128
		TB_Node *hiB, *loB;
		if (!shr) {
			hiB = tb_inst_shl(f, a0, bits_m64, ab);
			loB = zero;
		} else {
			loB = arith ? tb_inst_sar(f, a1, bits_m64) : tb_inst_shr(f, a1, bits_m64);
			hiB = sign;
		}

		TB_Node *hi = tb_inst_select(f, lt64, tb_inst_select(f, is0, a1, hiA), hiB);
		TB_Node *lo = tb_inst_select(f, lt64, tb_inst_select(f, is0, a0, loA), loB);
		TB_Node *of_hi = arith ? sign : zero;
		TB_Node *of_lo = arith ? sign : zero;
		r1 = tb_inst_select(f, ge128, of_hi, hi);
		r0 = tb_inst_select(f, ge128, of_lo, lo);
		break;
	}

	default:
		GB_PANIC("cg_emit_arith_u128: unhandled op %s", token_strings[op].text);
	}

	GB_ASSERT(r0 != nullptr && r1 != nullptr);
	return cg_u128_make_value(p, type, swapped, r0, r1);
}

gb_internal cgValue cg_emit_comp_u128(cgProcedure *p, TokenKind op_kind, cgValue left, cgValue right) {
	right = cg_emit_conv(p, right, left.type);
	left  = cg_emit_conv(p, left, right.type);

	Type *type = left.type;
	TB_Function *f = p->func;
	bool swapped = cg_u128_is_swapped(type);
	bool is_signed = !is_type_unsigned(type);

	TB_Node *lp = cg_u128_addr_of(p, left);
	TB_Node *rp = cg_u128_addr_of(p, right);
	TB_Node *a0 = cg_emit_u128_limb_load(p, lp, swapped, 0);
	TB_Node *a1 = cg_emit_u128_limb_load(p, lp, swapped, 1);
	TB_Node *b0 = cg_emit_u128_limb_load(p, rp, swapped, 0);
	TB_Node *b1 = cg_emit_u128_limb_load(p, rp, swapped, 1);

	TB_Node *lo_eq = tb_inst_cmp_eq(f, a0, b0);
	TB_Node *hi_eq = tb_inst_cmp_eq(f, a1, b1);
	TB_Node *eq = tb_inst_and(f, lo_eq, hi_eq);

	TB_Node *res = nullptr;
	switch (op_kind) {
	case Token_CmpEq:
		res = eq;
		break;
	case Token_NotEq:
		res = tb_inst_or(f, tb_inst_cmp_ne(f, a0, b0), tb_inst_cmp_ne(f, a1, b1));
		break;
	default: {
		TB_Node *hi_lt = tb_inst_cmp_ilt(f, a1, b1, is_signed);
		TB_Node *lo_lt = tb_inst_cmp_ilt(f, a0, b0, false);
		TB_Node *lt = tb_inst_or(f, hi_lt, tb_inst_and(f, hi_eq, lo_lt));

		TB_Node *hi_gt = tb_inst_cmp_igt(f, a1, b1, is_signed);
		TB_Node *lo_gt = tb_inst_cmp_igt(f, a0, b0, false);
		TB_Node *gt = tb_inst_or(f, hi_gt, tb_inst_and(f, hi_eq, lo_gt));

		switch (op_kind) {
		case Token_Lt:   res = lt;                         break;
		case Token_Gt:   res = gt;                         break;
		case Token_LtEq: res = tb_inst_or(f, lt, eq);      break;
		case Token_GtEq: res = tb_inst_or(f, gt, eq);      break;
		}
		break;
	}
	}
	GB_ASSERT(res != nullptr);
	return cg_value(res, t_bool);
}

gb_internal cgValue cg_emit_conv_int128(cgProcedure *p, cgValue value, Type *src, Type *dst, Type *t) {
	TB_Function *f = p->func;
	i64 sz = type_size_of(default_type(src));
	i64 dz = type_size_of(default_type(dst));
	GB_ASSERT(sz == 16 || dz == 16);

	if (sz == 16 && dz == 16) {
		if (types_have_same_internal_endian(src, dst)) {
			value.type = t;
			return value;
		}
		// endianness change: load both limbs (undoing the source swap) and
		// store them with the destination swap
		bool s_swap = cg_u128_is_swapped(src);
		bool d_swap = cg_u128_is_swapped(dst);
		TB_Node *ap = cg_u128_addr_of(p, value);
		TB_Node *lo = cg_emit_u128_limb_load(p, ap, s_swap, 0);
		TB_Node *hi = cg_emit_u128_limb_load(p, ap, s_swap, 1);
		return cg_u128_make_value(p, t, d_swap, lo, hi);
	}

	if (dz == 16) {
		// small integer -> 128-bit: widen to 64 bits with the source
		// signedness, then store {value, sign_extension}
		if (is_type_different_to_arch_endianness(src)) {
			Type *platform_src = integer_endian_type_to_platform_type(src);
			value = cg_emit_byte_swap(p, value, platform_src);
		}
		value = cg_flatten_value(p, value);
		GB_ASSERT(value.kind == cgValue_Value);
		TB_Node *v = value.node;
		bool src_unsigned = is_type_unsigned(src) || is_type_boolean(src);
		if (v->dt.type == TB_TAG_BOOL) {
			v = tb_inst_zxt(f, v, TB_TYPE_I64);
		} else if (v->dt.raw != TB_TYPE_I64.raw) {
			v = src_unsigned ? tb_inst_zxt(f, v, TB_TYPE_I64) : tb_inst_sxt(f, v, TB_TYPE_I64);
		}
		TB_Node *hi = nullptr;
		if (src_unsigned) {
			hi = tb_inst_uint(f, TB_TYPE_I64, 0);
		} else {
			hi = tb_inst_sar(f, v, tb_inst_uint(f, TB_TYPE_I64, 63));
		}
		bool d_swap = cg_u128_is_swapped(dst);
		return cg_u128_make_value(p, t, d_swap, v, hi);
	}

	// 128-bit -> small integer: keep the low limb, then narrow
	bool s_swap = cg_u128_is_swapped(src);
	TB_Node *ap = cg_u128_addr_of(p, value);
	TB_Node *lo = cg_emit_u128_limb_load(p, ap, s_swap, 0);
	cgValue lo_v = cg_value(lo, t_u64);
	if (dz > 1 && is_type_different_to_arch_endianness(dst)) {
		Type *platform_dst = integer_endian_type_to_platform_type(dst);
		cgValue r = cg_emit_conv(p, lo_v, platform_dst);
		return cg_emit_byte_swap(p, r, t);
	}
	return cg_emit_conv(p, lo_v, t);
}

gb_internal cgValue cg_emit_comp_records(cgProcedure *p, TokenKind op_kind, cgValue left, cgValue right, Type *type) {
	GB_ASSERT((is_type_struct(type) || is_type_union(type)) && is_type_comparable(type));
	cgValue left_ptr  = cg_address_from_load_or_generate_local(p, left);
	cgValue right_ptr = cg_address_from_load_or_generate_local(p, right);
	cgValue res = {};
	if (type_size_of(type) == 0) {
		switch (op_kind) {
		case Token_CmpEq:
			return cg_const_bool(p, t_bool, true);
		case Token_NotEq:
			return cg_const_bool(p, t_bool, false);
		}
		GB_PANIC("invalid operator");
	}
	TEMPORARY_ALLOCATOR_GUARD();
	if (is_type_simple_compare(type)) {
		// TODO(bill): Test to see if this is actually faster!!!!
		auto args = slice_make<cgValue>(temporary_allocator(), 3);
		args[0] = cg_emit_conv(p, left_ptr, t_rawptr);
		args[1] = cg_emit_conv(p, right_ptr, t_rawptr);
		args[2] = cg_const_int(p, t_int, type_size_of(type));
		res = cg_emit_runtime_call(p, "memory_equal", args);
	} else {
		cgProcedure *equal_proc = cg_equal_proc_for_type(p->module, type);
		cgValue value = cg_value(tb_inst_get_symbol_address(p->func, equal_proc->symbol), equal_proc->type);
		auto args = slice_make<cgValue>(temporary_allocator(), 2);
		args[0] = cg_emit_conv(p, left_ptr, t_rawptr);
		args[1] = cg_emit_conv(p, right_ptr, t_rawptr);
		res = cg_emit_call(p, value, args);
	}
	if (op_kind == Token_NotEq) {
		res = cg_emit_unary_arith(p, Token_Not, res, res.type);
	}
	return res;
}

gb_internal cgValue cg_emit_comp(cgProcedure *p, TokenKind op_kind, cgValue left, cgValue right) {
	GB_ASSERT(gb_is_between(op_kind, Token__ComparisonBegin+1, Token__ComparisonEnd-1));

	Type *a = core_type(left.type);
	Type *b = core_type(right.type);

	cgValue nil_check = {};
	if (is_type_array_like(left.type) || is_type_array_like(right.type)) {
		// don't do `nil` check if it is array-like
	} else if (is_type_untyped_nil(left.type)) {
		nil_check = cg_emit_comp_against_nil(p, op_kind, right);
	} else if (is_type_untyped_nil(right.type)) {
		nil_check = cg_emit_comp_against_nil(p, op_kind, left);
	}
	if (nil_check.node != nullptr) {
		return nil_check;
	}

	if (are_types_identical(a, b)) {
		// NOTE(bill): No need for a conversion
	} /*else if (cg_is_const(left) || cg_is_const_nil(left)) {
		left = cg_emit_conv(p, left, right.type);
	} else if (cg_is_const(right) || cg_is_const_nil(right)) {
		right = cg_emit_conv(p, right, left.type);
	}*/ else {
		Type *lt = left.type;
		Type *rt = right.type;

		lt = left.type;
		rt = right.type;
		i64 ls = type_size_of(lt);
		i64 rs = type_size_of(rt);

		// NOTE(bill): Quick heuristic, larger types are usually the target type
		if (ls < rs) {
			left = cg_emit_conv(p, left, rt);
		} else if (ls > rs) {
			right = cg_emit_conv(p, right, lt);
		} else {
			if (is_type_union(rt)) {
				left = cg_emit_conv(p, left, rt);
			} else {
				right = cg_emit_conv(p, right, lt);
			}
		}
	}

	a = core_type(left.type);
	b = core_type(right.type);
	left  = cg_flatten_value(p, left);
	right = cg_flatten_value(p, right);

	if (cg_is_type_integer_128(a)) {
		// NOTE(odin-tilde): 128-bit integers are memory aggregates here
		return cg_emit_comp_u128(p, op_kind, left, right);
	}


	if (is_type_matrix(a) && (op_kind == Token_CmpEq || op_kind == Token_NotEq)) {
		GB_PANIC("TODO(bill): cg_emit_comp matrix");
		// Type *tl = base_type(a);
		// lbValue lhs = lb_address_from_load_or_generate_local(p, left);
		// lbValue rhs = lb_address_from_load_or_generate_local(p, right);


		// // TODO(bill): Test to see if this is actually faster!!!!
		// auto args = array_make<lbValue>(permanent_allocator(), 3);
		// args[0] = lb_emit_conv(p, lhs, t_rawptr);
		// args[1] = lb_emit_conv(p, rhs, t_rawptr);
		// args[2] = lb_const_int(p->module, t_int, type_size_of(tl));
		// lbValue val = lb_emit_runtime_call(p, "memory_compare", args);
		// lbValue res = lb_emit_comp(p, op_kind, val, lb_const_nil(p->module, val.type));
		// return lb_emit_conv(p, res, t_bool);
	}
	if (is_type_array_like(a)) {
		// NOTE(odin-tilde): mirror of the LLVM strategy — memcmp for
		// simple-compare arrays, otherwise an inline element-wise gather.
		Type *tl = base_type(a);
		GB_ASSERT(op_kind == Token_CmpEq || op_kind == Token_NotEq);

		cgValue lhs = cg_address_from_load_or_generate_local(p, left);
		cgValue rhs = cg_address_from_load_or_generate_local(p, right);

		if (is_type_simple_compare(tl)) {
			auto args = slice_make<cgValue>(permanent_allocator(), 3);
			args[0] = cg_emit_conv(p, lhs, t_rawptr);
			args[1] = cg_emit_conv(p, rhs, t_rawptr);
			args[2] = cg_const_int(p, t_int, type_size_of(tl));
			cgValue val = cg_emit_runtime_call(p, "memory_compare", args);
			cgValue res = cg_emit_comp(p, op_kind, val, cg_const_int(p, val.type, 0));
			return cg_emit_conv(p, res, t_bool);
		}

		i32 count = 0;
		switch (tl->kind) {
		case Type_Array:           count = cast(i32)tl->Array.count;           break;
		case Type_EnumeratedArray: count = cast(i32)tl->EnumeratedArray.count; break;
		}
		GB_ASSERT(count >= 0);

		cgValue res = cg_const_bool(p, t_bool, op_kind == Token_CmpEq);
		TokenKind cmp_op = (op_kind == Token_CmpEq) ? Token_And : Token_Or;
		for (i32 i = 0; i < count; i++) {
			cgValue x = cg_emit_load(p, cg_emit_array_epi(p, lhs, i));
			cgValue y = cg_emit_load(p, cg_emit_array_epi(p, rhs, i));
			cgValue cmp = cg_emit_comp(p, op_kind, x, y);
			cmp = cg_emit_conv(p, cmp, t_bool);
			res = cg_emit_arith(p, cmp_op, res, cmp, t_bool);
		}
		return res;
	}
		// Type *tl = base_type(a);
		// lbValue lhs = lb_address_from_load_or_generate_local(p, left);
		// lbValue rhs = lb_address_from_load_or_generate_local(p, right);


		// TokenKind cmp_op = Token_And;
		// lbValue res = lb_const_bool(p->module, t_bool, true);
		// if (op_kind == Token_NotEq) {
		// 	res = lb_const_bool(p->module, t_bool, false);
		// 	cmp_op = Token_Or;
		// } else if (op_kind == Token_CmpEq) {
		// 	res = lb_const_bool(p->module, t_bool, true);
		// 	cmp_op = Token_And;
		// }

		// bool inline_array_arith = lb_can_try_to_inline_array_arith(tl);
		// i32 count = 0;
		// switch (tl->kind) {
		// case Type_Array:           count = cast(i32)tl->Array.count;           break;
		// case Type_EnumeratedArray: count = cast(i32)tl->EnumeratedArray.count; break;
		// }

		// if (inline_array_arith) {
		// 	// inline
		// 	lbAddr val = lb_add_local_generated(p, t_bool, false);
		// 	lb_addr_store(p, val, res);
		// 	for (i32 i = 0; i < count; i++) {
		// 		lbValue x = lb_emit_load(p, lb_emit_array_epi(p, lhs, i));
		// 		lbValue y = lb_emit_load(p, lb_emit_array_epi(p, rhs, i));
		// 		lbValue cmp = lb_emit_comp(p, op_kind, x, y);
		// 		lbValue new_res = lb_emit_arith(p, cmp_op, lb_addr_load(p, val), cmp, t_bool);
		// 		lb_addr_store(p, val, lb_emit_conv(p, new_res, t_bool));
		// 	}

		// 	return lb_addr_load(p, val);
		// } else {
		// 	if (is_type_simple_compare(tl) && (op_kind == Token_CmpEq || op_kind == Token_NotEq)) {
		// 		// TODO(bill): Test to see if this is actually faster!!!!
		// 		auto args = array_make<lbValue>(permanent_allocator(), 3);
		// 		args[0] = lb_emit_conv(p, lhs, t_rawptr);
		// 		args[1] = lb_emit_conv(p, rhs, t_rawptr);
		// 		args[2] = lb_const_int(p->module, t_int, type_size_of(tl));
		// 		lbValue val = lb_emit_runtime_call(p, "memory_compare", args);
		// 		lbValue res = lb_emit_comp(p, op_kind, val, lb_const_nil(p->module, val.type));
		// 		return lb_emit_conv(p, res, t_bool);
		// 	} else {
		// 		lbAddr val = lb_add_local_generated(p, t_bool, false);
		// 		lb_addr_store(p, val, res);
		// 		auto loop_data = lb_loop_start(p, count, t_i32);
		// 		{
		// 			lbValue i = loop_data.idx;
		// 			lbValue x = lb_emit_load(p, lb_emit_array_ep(p, lhs, i));
		// 			lbValue y = lb_emit_load(p, lb_emit_array_ep(p, rhs, i));
		// 			lbValue cmp = lb_emit_comp(p, op_kind, x, y);
		// 			lbValue new_res = lb_emit_arith(p, cmp_op, lb_addr_load(p, val), cmp, t_bool);
		// 			lb_addr_store(p, val, lb_emit_conv(p, new_res, t_bool));
		// 		}
		// 		lb_loop_end(p, loop_data);

		// 		return lb_addr_load(p, val);
		// 	}
		// }

	if ((is_type_struct(a) || is_type_union(a)) && is_type_comparable(a)) {
		return cg_emit_comp_records(p, op_kind, left, right, a);
	}

	if ((is_type_struct(b) || is_type_union(b)) && is_type_comparable(b)) {
		return cg_emit_comp_records(p, op_kind, left, right, b);
	}

	if (is_type_string16(a) || is_type_cstring16(a)) {
		// NOTE(odin-tilde): ported from lb_emit_comp — wide strings have their
		// own runtime comparisons; never mix with the UTF-8 string_eq family.
		if (is_type_cstring16(a) && is_type_cstring16(b)) {
			left  = cg_emit_conv(p, left, t_cstring16);
			right = cg_emit_conv(p, right, t_cstring16);
			char const *runtime_procedure = nullptr;
			switch (op_kind) {
			case Token_CmpEq: runtime_procedure = "cstring16_eq"; break;
			case Token_NotEq: runtime_procedure = "cstring16_ne"; break;
			case Token_Lt:    runtime_procedure = "cstring16_lt"; break;
			case Token_Gt:    runtime_procedure = "cstring16_gt"; break;
			case Token_LtEq:  runtime_procedure = "cstring16_le"; break;
			case Token_GtEq:  runtime_procedure = "cstring16_ge"; break;
			}
			GB_ASSERT(runtime_procedure != nullptr);

			auto args = slice_make<cgValue>(permanent_allocator(), 2);
			args[0] = left;
			args[1] = right;
			return cg_emit_runtime_call(p, runtime_procedure, args);
		}

		if (is_type_cstring16(a) ^ is_type_cstring16(b)) {
			left  = cg_emit_conv(p, left, t_string16);
			right = cg_emit_conv(p, right, t_string16);
		}

		char const *runtime_procedure = nullptr;
		switch (op_kind) {
		case Token_CmpEq: runtime_procedure = "string16_eq"; break;
		case Token_NotEq: runtime_procedure = "string16_ne"; break;
		case Token_Lt:    runtime_procedure = "string16_lt"; break;
		case Token_Gt:    runtime_procedure = "string16_gt"; break;
		case Token_LtEq:  runtime_procedure = "string16_le"; break;
		case Token_GtEq:  runtime_procedure = "string16_ge"; break;
		}
		GB_ASSERT(runtime_procedure != nullptr);

		auto args = slice_make<cgValue>(permanent_allocator(), 2);
		args[0] = left;
		args[1] = right;
		return cg_emit_runtime_call(p, runtime_procedure, args);
	}

	if (is_type_string(a)) {
		if (is_type_cstring(a)) {
			left  = cg_emit_conv(p, left, t_string);
			right = cg_emit_conv(p, right, t_string);
		}

		char const *runtime_procedure = nullptr;
		switch (op_kind) {
		case Token_CmpEq: runtime_procedure = "string_eq"; break;
		case Token_NotEq: runtime_procedure = "string_ne"; break;
		case Token_Lt:    runtime_procedure = "string_lt"; break;
		case Token_Gt:    runtime_procedure = "string_gt"; break;
		case Token_LtEq:  runtime_procedure = "string_le"; break;
		case Token_GtEq:  runtime_procedure = "string_gt"; break;
		}
		GB_ASSERT(runtime_procedure != nullptr);

		auto args = slice_make<cgValue>(permanent_allocator(), 2);
		args[0] = left;
		args[1] = right;
		return cg_emit_runtime_call(p, runtime_procedure, args);
	}

	if (is_type_complex(a)) {
		char const *runtime_procedure = "";
		i64 sz = 8*type_size_of(a);
		switch (sz) {
		case 32:
			switch (op_kind) {
			case Token_CmpEq: runtime_procedure = "complex32_eq"; break;
			case Token_NotEq: runtime_procedure = "complex32_ne"; break;
			}
			break;
		case 64:
			switch (op_kind) {
			case Token_CmpEq: runtime_procedure = "complex64_eq"; break;
			case Token_NotEq: runtime_procedure = "complex64_ne"; break;
			}
			break;
		case 128:
			switch (op_kind) {
			case Token_CmpEq: runtime_procedure = "complex128_eq"; break;
			case Token_NotEq: runtime_procedure = "complex128_ne"; break;
			}
			break;
		}
		GB_ASSERT(runtime_procedure != nullptr);

		auto args = slice_make<cgValue>(permanent_allocator(), 2);
		args[0] = left;
		args[1] = right;
		return cg_emit_runtime_call(p, runtime_procedure, args);
	}

	if (is_type_quaternion(a)) {
		char const *runtime_procedure = "";
		i64 sz = 8*type_size_of(a);
		switch (sz) {
		case 64:
			switch (op_kind) {
			case Token_CmpEq: runtime_procedure = "quaternion64_eq"; break;
			case Token_NotEq: runtime_procedure = "quaternion64_ne"; break;
			}
			break;
		case 128:
			switch (op_kind) {
			case Token_CmpEq: runtime_procedure = "quaternion128_eq"; break;
			case Token_NotEq: runtime_procedure = "quaternion128_ne"; break;
			}
			break;
		case 256:
			switch (op_kind) {
			case Token_CmpEq: runtime_procedure = "quaternion256_eq"; break;
			case Token_NotEq: runtime_procedure = "quaternion256_ne"; break;
			}
			break;
		}
		GB_ASSERT(runtime_procedure != nullptr);

		auto args = slice_make<cgValue>(permanent_allocator(), 2);
		args[0] = left;
		args[1] = right;
		return cg_emit_runtime_call(p, runtime_procedure, args);
	}

	if (is_type_bit_set(a)) {
		switch (op_kind) {
		case Token_Lt:
		case Token_LtEq:
		case Token_Gt:
		case Token_GtEq:
			{
				Type *it = bit_set_to_int(a);
				cgValue lhs = cg_emit_transmute(p, left, it);
				cgValue rhs = cg_emit_transmute(p, right, it);
				cgValue res = cg_emit_arith(p, Token_And, lhs, rhs, it);
				GB_ASSERT(lhs.kind == cgValue_Value);
				GB_ASSERT(rhs.kind == cgValue_Value);
				GB_ASSERT(res.kind == cgValue_Value);

				if (op_kind == Token_Lt || op_kind == Token_LtEq) {
					// (lhs & rhs) == lhs
					res = cg_value(tb_inst_cmp_eq(p->func, res.node, lhs.node), t_bool);
				} else if (op_kind == Token_Gt || op_kind == Token_GtEq) {
					// (lhs & rhs) == rhs
					res = cg_value(tb_inst_cmp_eq(p->func, res.node, rhs.node), t_bool);
				}

				// NOTE(bill): Strict subsets
				if (op_kind == Token_Lt || op_kind == Token_Gt) {
					// res &~ (lhs == rhs)
					cgValue eq = cg_value(tb_inst_cmp_eq(p->func, lhs.node, rhs.node), t_bool);
					res = cg_emit_arith(p, Token_AndNot, res, eq, t_bool);
				}
				return res;
			}

		case Token_CmpEq:
			GB_ASSERT(left.kind  == cgValue_Value);
			GB_ASSERT(right.kind == cgValue_Value);
			return cg_value(tb_inst_cmp_eq(p->func, left.node, right.node), t_bool);
		case Token_NotEq:
			GB_ASSERT(left.kind  == cgValue_Value);
			GB_ASSERT(right.kind == cgValue_Value);
			return cg_value(tb_inst_cmp_ne(p->func, left.node, right.node), t_bool);
		}
	}

	if (op_kind != Token_CmpEq && op_kind != Token_NotEq) {
		Type *t = left.type;
		if (is_type_integer(t) && is_type_different_to_arch_endianness(t)) {
			Type *platform_type = integer_endian_type_to_platform_type(t);
			cgValue x = cg_emit_byte_swap(p, left, platform_type);
			cgValue y = cg_emit_byte_swap(p, right, platform_type);
			left = x;
			right = y;
		} else if (is_type_float(t) && is_type_different_to_arch_endianness(t)) {
			Type *platform_type = integer_endian_type_to_platform_type(t);
			cgValue x = cg_emit_conv(p, left, platform_type);
			cgValue y = cg_emit_conv(p, right, platform_type);
			left = x;
			right = y;
		}
	}

	a = core_type(left.type);
	b = core_type(right.type);


	if (is_type_integer(a) ||
	    is_type_boolean(a) ||
	    is_type_pointer(a) ||
	    is_type_multi_pointer(a) ||
	    is_type_proc(a) ||
	    is_type_enum(a) ||
	    is_type_typeid(a)) {
	    	TB_Node *lhs = left.node;
		TB_Node *rhs = right.node;
		TB_Node *res = nullptr;

		// PATCH(odin-tilde): identical Odin types can still have different TB
		// representations — comparison results are TB_TAG_BOOL while bool
		// constants/loads are INTN. Widen BOOL to the other side's type.
		if (lhs->dt.raw != rhs->dt.raw) {
			if (lhs->dt.type == TB_TAG_BOOL && TB_IS_INTEGER_TYPE(rhs->dt)) {
				lhs = tb_inst_zxt(p->func, lhs, rhs->dt);
			} else if (rhs->dt.type == TB_TAG_BOOL && TB_IS_INTEGER_TYPE(lhs->dt)) {
				rhs = tb_inst_zxt(p->func, rhs, lhs->dt);
			}
		}

		// PATCH(odin-tilde): is_type_integer is false for enums, which used to
		// silently select the unsigned form; enums compare with the signedness
		// of their base integer type (e.g. Ordering's -1 must compare signed).
		bool is_signed = false;
		if (is_type_integer(left.type)) {
			is_signed = !is_type_unsigned(left.type);
		} else if (is_type_enum(left.type)) {
			Type *enum_base = base_type(left.type)->Enum.base_type;
			is_signed = is_type_integer(enum_base) && !is_type_unsigned(enum_base);
		}
		switch (op_kind) {
		case Token_CmpEq: res = tb_inst_cmp_eq(p->func, lhs, rhs); break;
		case Token_NotEq: res = tb_inst_cmp_ne(p->func, lhs, rhs); break;
		case Token_Gt:    res = tb_inst_cmp_igt(p->func, lhs, rhs, is_signed); break;
		case Token_GtEq:  res = tb_inst_cmp_ige(p->func, lhs, rhs, is_signed); break;
		case Token_Lt:    res = tb_inst_cmp_ilt(p->func, lhs, rhs, is_signed); break;
		case Token_LtEq:  res = tb_inst_cmp_ile(p->func, lhs, rhs, is_signed); break;
		}

		GB_ASSERT(res != nullptr);
		return cg_value(res, t_bool);
	} else if (is_type_float(a)) {
	    	TB_Node *lhs = left.node;
		TB_Node *rhs = right.node;
		// NOTE(odin-tilde): f16 is kept as an I16 bit pattern (TB has no f16
		// float ops). Comparing bit patterns as integers would give wrong
		// float semantics (NaN, -0.0), so promote both sides to f32 first —
		// the widening is exact and preserves f16 comparison semantics.
		if (type_size_of(a) == 2) {
			lhs = cg_emit_conv(p, left,  t_f32).node;
			rhs = cg_emit_conv(p, right, t_f32).node;
		}
		TB_Node *res = nullptr;
		switch (op_kind) {
		case Token_CmpEq: res = tb_inst_cmp_eq(p->func, lhs, rhs);  break;
		case Token_NotEq: res = tb_inst_cmp_ne(p->func, lhs, rhs);  break;
		case Token_Gt:    res = tb_inst_cmp_fgt(p->func, lhs, rhs); break;
		case Token_GtEq:  res = tb_inst_cmp_fge(p->func, lhs, rhs); break;
		case Token_Lt:    res = tb_inst_cmp_flt(p->func, lhs, rhs); break;
		case Token_LtEq:  res = tb_inst_cmp_fle(p->func, lhs, rhs); break;
		}
		GB_ASSERT(res != nullptr);
		return cg_value(res, t_bool);
	} else if (is_type_simd_vector(a)) {
		GB_PANIC("TODO(bill): #simd vector");
		// LLVMValueRef mask = nullptr;
		// Type *elem = base_array_type(a);
		// if (is_type_float(elem)) {
		// 	LLVMRealPredicate pred = {};
		// 	switch (op_kind) {
		// 	case Token_CmpEq: pred = LLVMRealOEQ; break;
		// 	case Token_NotEq: pred = LLVMRealONE; break;
		// 	}
		// 	mask = LLVMBuildFCmp(p->builder, pred, left.value, right.value, "");
		// } else {
		// 	LLVMIntPredicate pred = {};
		// 	switch (op_kind) {
		// 	case Token_CmpEq: pred = LLVMIntEQ; break;
		// 	case Token_NotEq: pred = LLVMIntNE; break;
		// 	}
		// 	mask = LLVMBuildICmp(p->builder, pred, left.value, right.value, "");
		// }
		// GB_ASSERT_MSG(mask != nullptr, "Unhandled comparison kind %s (%s) %.*s %s (%s)", type_to_string(left.type), type_to_string(base_type(left.type)), LIT(token_strings[op_kind]), type_to_string(right.type), type_to_string(base_type(right.type)));

		// /* NOTE(bill, 2022-05-28):
		// 	Thanks to Per Vognsen, sign extending <N x i1> to
		// 	a vector of the same width as the input vector, bit casting to an integer,
		// 	and then comparing against zero is the better option
		// 	See: https://lists.llvm.org/pipermail/llvm-dev/2012-September/053046.html

		// 	// Example assuming 128-bit vector

		// 	%1 = <4 x float> ...
		// 	%2 = <4 x float> ...
		// 	%3 = fcmp oeq <4 x float> %1, %2
		// 	%4 = sext <4 x i1> %3 to <4 x i32>
		// 	%5 = bitcast <4 x i32> %4 to i128
		// 	%6 = icmp ne i128 %5, 0
		// 	br i1 %6, label %true1, label %false2

		// 	This will result in 1 cmpps + 1 ptest + 1 br
		// 	(even without SSE4.1, contrary to what the mail list states, because of pmovmskb)

		// */

		// unsigned count = cast(unsigned)get_array_type_count(a);
		// unsigned elem_sz = cast(unsigned)(type_size_of(elem)*8);
		// LLVMTypeRef mask_type = LLVMVectorType(LLVMIntTypeInContext(p->module->ctx, elem_sz), count);
		// mask = LLVMBuildSExtOrBitCast(p->builder, mask, mask_type, "");

		// LLVMTypeRef mask_int_type = LLVMIntTypeInContext(p->module->ctx, cast(unsigned)(8*type_size_of(a)));
		// LLVMValueRef mask_int = LLVMBuildBitCast(p->builder, mask, mask_int_type, "");
		// res.value = LLVMBuildICmp(p->builder, LLVMIntNE, mask_int, LLVMConstNull(LLVMTypeOf(mask_int)), "");
		// return res;
	}

	GB_PANIC("Unhandled comparison kind %s (%s) %.*s %s (%s)", type_to_string(left.type), type_to_string(base_type(left.type)), LIT(token_strings[op_kind]), type_to_string(right.type), type_to_string(base_type(right.type)));
	return {};
}

gb_internal cgValue cg_emit_comp_against_nil(cgProcedure *p, TokenKind op_kind, cgValue x) {
	GB_ASSERT(op_kind == Token_CmpEq || op_kind == Token_NotEq);
	x = cg_flatten_value(p, x);
	cgValue res = {};
	Type *t = x.type;

	TB_DataType dt = cg_data_type(t);

	Type *bt = base_type(t);
	TypeKind type_kind = bt->kind;

	switch (type_kind) {
	case Type_Basic:
		switch (bt->Basic.kind) {
		case Basic_rawptr:
		case Basic_cstring:
		case Basic_cstring16:
			GB_ASSERT(x.kind == cgValue_Value);
			if (op_kind == Token_CmpEq) {
				return cg_value(tb_inst_cmp_eq(p->func, x.node, tb_inst_uint(p->func, dt, 0)), t_bool);
			} else if (op_kind == Token_NotEq) {
				return cg_value(tb_inst_cmp_ne(p->func, x.node, tb_inst_uint(p->func, dt, 0)), t_bool);
			}
			break;
		case Basic_any:
			{
				GB_ASSERT(x.kind == cgValue_Addr);
				// // TODO(bill): is this correct behaviour for nil comparison for any?
				cgValue data = cg_emit_struct_ev(p, x, 0);
				cgValue id   = cg_emit_struct_ev(p, x, 1);

				if (op_kind == Token_CmpEq) {
					TB_Node *a = tb_inst_cmp_eq(p->func, data.node, tb_inst_uint(p->func, data.node->dt, 0));
					TB_Node *b = tb_inst_cmp_eq(p->func, id.node,   tb_inst_uint(p->func, id.node->dt,   0));
					TB_Node *c = tb_inst_or(p->func, a, b);
					return cg_value(c, t_bool);
				} else if (op_kind == Token_NotEq) {
					TB_Node *a = tb_inst_cmp_ne(p->func, data.node, tb_inst_uint(p->func, data.node->dt, 0));
					TB_Node *b = tb_inst_cmp_ne(p->func, id.node,   tb_inst_uint(p->func, id.node->dt,   0));
					TB_Node *c = tb_inst_and(p->func, a, b);
					return cg_value(c, t_bool);
				}
			}
			break;
		case Basic_typeid:
			cgValue invalid_typeid = cg_const_value(p, t_typeid, exact_value_i64(0));
			return cg_emit_comp(p, op_kind, x, invalid_typeid);
		}
		break;

	case Type_Enum:
	case Type_Pointer:
	case Type_MultiPointer:
	case Type_Proc:
	case Type_BitSet:
		GB_ASSERT(x.kind == cgValue_Value);
		if (op_kind == Token_CmpEq) {
			return cg_value(tb_inst_cmp_eq(p->func, x.node, tb_inst_uint(p->func, dt, 0)), t_bool);
		} else if (op_kind == Token_NotEq) {
			return cg_value(tb_inst_cmp_ne(p->func, x.node, tb_inst_uint(p->func, dt, 0)), t_bool);
		}
		break;

	case Type_Slice:
	case Type_DynamicArray:
	case Type_Map:
		{
			// NOTE(bill): all of their data "pointer-like" fields are at the 0-index
			cgValue data = cg_emit_struct_ev(p, x, 0);
			if (op_kind == Token_CmpEq) {
				TB_Node *a = tb_inst_cmp_eq(p->func, data.node, tb_inst_uint(p->func, data.node->dt, 0));
				return cg_value(a, t_bool);
			} else if (op_kind == Token_NotEq) {
				TB_Node *a = tb_inst_cmp_ne(p->func, data.node, tb_inst_uint(p->func, data.node->dt, 0));
				return cg_value(a, t_bool);
			}
		}
		break;

	case Type_Union:
		{
			if (type_size_of(t) == 0) {
				return cg_const_bool(p, t_bool, op_kind == Token_CmpEq);
			} else if (is_type_union_maybe_pointer(t)) {
				cgValue tag = cg_emit_transmute(p, x, t_rawptr);
				return cg_emit_comp_against_nil(p, op_kind, tag);
			} else {
				cgValue x_ptr = cg_address_from_load_or_generate_local(p, x);
				cgValue tag = cg_emit_load(p, cg_emit_union_tag_ptr(p, x_ptr));
				return cg_emit_comp(p, op_kind, tag, cg_const_int(p, tag.type, 0));
			}
		}
		break;
	case Type_Struct:
		GB_PANIC("TODO(bill): cg_emit_struct_ev");
		// if (is_type_soa_struct(t)) {
		// 	Type *bt = base_type(t);
		// 	if (bt->Struct.soa_kind == StructSoa_Slice) {
		// 		LLVMValueRef the_value = {};
		// 		if (bt->Struct.fields.count == 0) {
		// 			cgValue len = cg_soa_struct_len(p, x);
		// 			the_value = len.value;
		// 		} else {
		// 			cgValue first_field = cg_emit_struct_ev(p, x, 0);
		// 			the_value = first_field.value;
		// 		}
		// 		if (op_kind == Token_CmpEq) {
		// 			res.value = LLVMBuildIsNull(p->builder, the_value, "");
		// 			return res;
		// 		} else if (op_kind == Token_NotEq) {
		// 			res.value = LLVMBuildIsNotNull(p->builder, the_value, "");
		// 			return res;
		// 		}
		// 	} else if (bt->Struct.soa_kind == StructSoa_Dynamic) {
		// 		LLVMValueRef the_value = {};
		// 		if (bt->Struct.fields.count == 0) {
		// 			cgValue cap = cg_soa_struct_cap(p, x);
		// 			the_value = cap.value;
		// 		} else {
		// 			cgValue first_field = cg_emit_struct_ev(p, x, 0);
		// 			the_value = first_field.value;
		// 		}
		// 		if (op_kind == Token_CmpEq) {
		// 			res.value = LLVMBuildIsNull(p->builder, the_value, "");
		// 			return res;
		// 		} else if (op_kind == Token_NotEq) {
		// 			res.value = LLVMBuildIsNotNull(p->builder, the_value, "");
		// 			return res;
		// 		}
		// 	}
		// } else if (is_type_struct(t) && type_has_nil(t)) {
		// 	auto args = array_make<cgValue>(permanent_allocator(), 2);
		// 	cgValue lhs = cg_address_from_load_or_generate_local(p, x);
		// 	args[0] = cg_emit_conv(p, lhs, t_rawptr);
		// 	args[1] = cg_const_int(p->module, t_int, type_size_of(t));
		// 	cgValue val = cg_emit_runtime_call(p, "memory_compare_zero", args);
		// 	cgValue res = cg_emit_comp(p, op_kind, val, cg_const_int(p->module, t_int, 0));
		// 	return res;
		// }
		break;
	}
	GB_PANIC("Unknown handled type: %s -> %s", type_to_string(t), type_to_string(bt));
	return {};
}

// NOTE(odin-tilde): TB has no f16 support; half-precision floats are kept as
// I16 bit patterns (TB_TYPE_F16 == TB_TYPE_I16 in the compat layer), so all
// f16 <-> float conversions are done in software with f32 as intermediate.

gb_internal TB_Node *cg_tb_bool_to_i32(TB_Function *f, TB_Node *cond) {
	return tb_inst_select(f, cond,
		tb_inst_uint(f, TB_TYPE_I32, 1),
		tb_inst_uint(f, TB_TYPE_I32, 0));
}

gb_internal TB_Node *cg_tb_f16_to_f32_node(TB_Function *f, TB_Node *bits16) {
	TB_Node *b = tb_inst_zxt(f, bits16, TB_TYPE_I32);

	TB_Node *sign = tb_inst_and(f, tb_inst_shr(f, b, tb_inst_uint(f, TB_TYPE_I32, 15)), tb_inst_uint(f, TB_TYPE_I32, 1));
	TB_Node *exp  = tb_inst_and(f, tb_inst_shr(f, b, tb_inst_uint(f, TB_TYPE_I32, 10)), tb_inst_uint(f, TB_TYPE_I32, 0x1f));
	TB_Node *mant = tb_inst_and(f, b, tb_inst_uint(f, TB_TYPE_I32, 0x3ff));
	TB_Node *sign31 = tb_inst_shl(f, sign, tb_inst_uint(f, TB_TYPE_I32, 31), TB_ARITHMATIC_NONE);

	// normal: rebias exponent (exp-15+127 = exp+112)
	TB_Node *normal = tb_inst_or(f,
		tb_inst_or(f, sign31,
			tb_inst_shl(f, tb_inst_add(f, exp, tb_inst_uint(f, TB_TYPE_I32, 112), TB_ARITHMATIC_NONE),
				tb_inst_uint(f, TB_TYPE_I32, 23), TB_ARITHMATIC_NONE)),
		tb_inst_shl(f, mant, tb_inst_uint(f, TB_TYPE_I32, 13), TB_ARITHMATIC_NONE));

	// inf/nan
	TB_Node *infnan = tb_inst_or(f,
		tb_inst_or(f, sign31, tb_inst_uint(f, TB_TYPE_I32, 0x7f800000)),
		tb_inst_shl(f, mant, tb_inst_uint(f, TB_TYPE_I32, 13), TB_ARITHMATIC_NONE));

	// subnormal/zero: exact via mant * 2^-24
	TB_Node *mantf = tb_inst_int2float(f, mant, TB_TYPE_F32, false);
	TB_Node *subf  = tb_inst_fmul(f, mantf, tb_inst_float32(f, 5.9604644775390625e-08f /* 2^-24 */));
	TB_Node *sub   = tb_inst_or(f, tb_inst_bitcast(f, subf, TB_TYPE_I32), sign31);

	TB_Node *is_zero_exp = tb_inst_cmp_eq(f, exp, tb_inst_uint(f, TB_TYPE_I32, 0));
	TB_Node *is_max_exp  = tb_inst_cmp_eq(f, exp, tb_inst_uint(f, TB_TYPE_I32, 0x1f));

	TB_Node *r = tb_inst_select(f, is_zero_exp, sub, tb_inst_select(f, is_max_exp, infnan, normal));
	return tb_inst_bitcast(f, r, TB_TYPE_F32);
}

gb_internal TB_Node *cg_tb_f32_to_f16_node(TB_Function *f, TB_Node *f32) {
	TB_Node *x = tb_inst_bitcast(f, f32, TB_TYPE_I32);

	TB_Node *sign = tb_inst_and(f, tb_inst_shr(f, x, tb_inst_uint(f, TB_TYPE_I32, 16)), tb_inst_uint(f, TB_TYPE_I32, 0x8000));
	TB_Node *ax   = tb_inst_and(f, x, tb_inst_uint(f, TB_TYPE_I32, 0x7fffffff));
	TB_Node *exp  = tb_inst_and(f, tb_inst_shr(f, ax, tb_inst_uint(f, TB_TYPE_I32, 23)), tb_inst_uint(f, TB_TYPE_I32, 0xff));
	TB_Node *mant = tb_inst_and(f, ax, tb_inst_uint(f, TB_TYPE_I32, 0x7fffff));

	TB_Node *zero = tb_inst_uint(f, TB_TYPE_I32, 0);
	TB_Node *one  = tb_inst_uint(f, TB_TYPE_I32, 1);

	// inf/nan input: keep nan as nan. The payload is mant>>13 (matching
	// vcvtps2ph/__gnu_f2h_ieee); only when those high payload bits are all
	// zero do we force the LSB so a NaN never degrades into an infinity.
	TB_Node *payload = tb_inst_shr(f, mant, tb_inst_uint(f, TB_TYPE_I32, 13));
	TB_Node *is_nan  = tb_inst_cmp_ne(f, mant, zero);
	TB_Node *guard   = tb_inst_and(f, cg_tb_bool_to_i32(f, is_nan),
		cg_tb_bool_to_i32(f, tb_inst_cmp_eq(f, payload, zero)));
	TB_Node *infnan = tb_inst_or(f, sign,
		tb_inst_or(f, tb_inst_uint(f, TB_TYPE_I32, 0x7c00),
			tb_inst_or(f, payload, guard)));

	// e = f16-biased exponent (signed)
	TB_Node *e = tb_inst_sub(f, exp, tb_inst_uint(f, TB_TYPE_I32, 112), TB_ARITHMATIC_NONE);

	// normal f16: (e<<10) | (mant>>13), round-to-nearest-even on the low 13 bits
	TB_Node *nhalf = tb_inst_or(f,
		tb_inst_shl(f, e, tb_inst_uint(f, TB_TYPE_I32, 10), TB_ARITHMATIC_NONE),
		tb_inst_shr(f, mant, tb_inst_uint(f, TB_TYPE_I32, 13)));
	TB_Node *nrem  = tb_inst_and(f, mant, tb_inst_uint(f, TB_TYPE_I32, 0x1fff));
	TB_Node *nup   = tb_inst_or(f,
		tb_inst_cmp_igt(f, nrem, tb_inst_uint(f, TB_TYPE_I32, 0x1000), false),
		tb_inst_and(f,
			tb_inst_cmp_eq(f, nrem, tb_inst_uint(f, TB_TYPE_I32, 0x1000)),
			tb_inst_cmp_ne(f, tb_inst_and(f, nhalf, one), zero)));
	TB_Node *normal = tb_inst_add(f, sign,
		tb_inst_add(f, nhalf, cg_tb_bool_to_i32(f, nup), TB_ARITHMATIC_NONE),
		TB_ARITHMATIC_NONE);

	// subnormal f16 (e <= 0): mantissa with implicit bit, shifted right with RNE
	TB_Node *mant_imp = tb_inst_or(f, mant, tb_inst_uint(f, TB_TYPE_I32, 0x800000));
	TB_Node *shift = tb_inst_sub(f, tb_inst_uint(f, TB_TYPE_I32, 14), e, TB_ARITHMATIC_NONE);
	// clamp so the shift amount stays well-defined even on the discarded path
	shift = tb_inst_select(f, tb_inst_cmp_igt(f, shift, tb_inst_uint(f, TB_TYPE_I32, 31), true),
		tb_inst_uint(f, TB_TYPE_I32, 31), shift);
	TB_Node *shalf   = tb_inst_shr(f, mant_imp, shift);
	TB_Node *pow2    = tb_inst_shl(f, one, shift, TB_ARITHMATIC_NONE);
	TB_Node *srem    = tb_inst_and(f, mant_imp, tb_inst_sub(f, pow2, one, TB_ARITHMATIC_NONE));
	TB_Node *halfway = tb_inst_shr(f, pow2, one);
	TB_Node *sup     = tb_inst_or(f,
		tb_inst_cmp_igt(f, srem, halfway, false),
		tb_inst_and(f,
			tb_inst_cmp_eq(f, srem, halfway),
			tb_inst_cmp_ne(f, tb_inst_and(f, shalf, one), zero)));
	TB_Node *sub = tb_inst_add(f, sign,
		tb_inst_add(f, shalf, cg_tb_bool_to_i32(f, sup), TB_ARITHMATIC_NONE),
		TB_ARITHMATIC_NONE);

	TB_Node *r = tb_inst_select(f, tb_inst_cmp_ile(f, e, zero, true), sub, normal);
	r = tb_inst_select(f, tb_inst_cmp_ige(f, e, tb_inst_uint(f, TB_TYPE_I32, 0x1f), true),
		tb_inst_or(f, sign, tb_inst_uint(f, TB_TYPE_I32, 0x7c00)), r);
	r = tb_inst_select(f, tb_inst_cmp_eq(f, exp, tb_inst_uint(f, TB_TYPE_I32, 0xff)), infnan, r);
	return tb_inst_trunc(f, r, TB_TYPE_I16);
}

gb_internal cgValue cg_emit_conv(cgProcedure *p, cgValue value, Type *t) {
	t = reduce_tuple_to_single_type(t);

	if (value.kind == cgValue_Multi && t != nullptr && is_type_tuple(base_type(t))) {
		// NOTE(odin-tilde): a multi-value result converted to a tuple type
		// (e.g. `a, b := f() or_break`, where the try lhs is every value
		// but the trailing condition); tuples are not first-class values
		// in this backend, so materialize them into a local.
		Type *bt = base_type(t);
		GB_ASSERT(value.multi->values.count == bt->Tuple.variables.count);
		cgAddr dst = cg_add_local(p, t, nullptr, false);
		cgValue ptr = cg_addr_get_ptr(p, dst);
		for_array(i, value.multi->values) {
			cgValue v = cg_emit_conv(p, value.multi->values[i], bt->Tuple.variables[i]->type);
			cg_emit_store(p, cg_emit_struct_ep(p, ptr, cast(i32)i), v);
		}
		return cg_addr_load(p, dst);
	}

	value = cg_flatten_value(p, value);

	Type *src_type = value.type;
	if (are_types_identical(t, src_type)) {
		return value;
	}

	if (is_type_untyped_uninit(src_type)) {
		// return cg_const_undef(m, t);
		return cg_const_nil(p, t);
	}
	if (is_type_untyped_nil(src_type)) {
		return cg_const_nil(p, t);
	}

	Type *src = core_type(src_type);
	Type *dst = core_type(t);
	GB_ASSERT_MSG(src != nullptr, "src_type=%s t=%s proc=%.*s", type_to_string(src_type), type_to_string(t), LIT(p->name));
	GB_ASSERT(dst != nullptr);

	if (are_types_identical(src, dst)) {
		return cg_emit_transmute(p, value, t);
	}

	TB_DataType st = cg_data_type(src);
	if (value.kind == cgValue_Value && !TB_IS_VOID_TYPE(value.node->dt)) {
		st = value.node->dt;
	}
	TB_DataType dt = cg_data_type(t);

	if (is_type_integer(src) && is_type_integer(dst)) {
		GB_ASSERT(src->kind == Type_Basic &&
		          dst->kind == Type_Basic);

		i64 sz = type_size_of(default_type(src));
		i64 dz = type_size_of(default_type(dst));

		if (sz == 16 || dz == 16) {
			// NOTE(odin-tilde): 128-bit integers are memory aggregates here
			return cg_emit_conv_int128(p, value, src, dst, t);
		}

		GB_ASSERT(value.kind == cgValue_Value);

		if (sz == dz) {
			if (dz > 1 && !types_have_same_internal_endian(src, dst)) {
				return cg_emit_byte_swap(p, value, t);
			}
			value.type = t;
			return value;
		}

		if (sz > 1 && is_type_different_to_arch_endianness(src)) {
			Type *platform_src_type = integer_endian_type_to_platform_type(src);
			value = cg_emit_byte_swap(p, value, platform_src_type);
		}

		TB_Node* (*op)(TB_Function* f, TB_Node* src, TB_DataType dt) = tb_inst_trunc;

		if (dz < sz) {
			op = tb_inst_trunc;
		} else if (dz == sz) {
			op = tb_inst_bitcast;
		} else if (dz > sz) {
			op = is_type_unsigned(src) ? tb_inst_zxt : tb_inst_sxt; // zero extent
		}

		if (dz > 1 && is_type_different_to_arch_endianness(dst)) {
			Type *platform_dst_type = integer_endian_type_to_platform_type(dst);

			cgValue res = cg_value(op(p->func, value.node, cg_data_type(platform_dst_type)), platform_dst_type);
			return cg_emit_byte_swap(p, res, t);
		} else {
			return cg_value(op(p->func, value.node, dt), t);
		}
	}

	// boolean -> boolean/integer
	if (is_type_boolean(src) && (is_type_boolean(dst) || is_type_integer(dst))) {
		if (type_size_of(dst) == 16) {
			// NOTE(odin-tilde): widen through u64 into the aggregate
			cgValue v64 = cg_emit_conv(p, value, t_u64);
			return cg_emit_conv_int128(p, v64, t_u64, dst, t);
		}
		TB_Node *v = tb_inst_cmp_ne(p->func, value.node, tb_inst_uint(p->func, st, 0));
		return cg_value(tb_inst_zxt(p->func, v, dt), t);
	}

	// integer -> boolean
	if (is_type_integer(src) && is_type_boolean(dst)) {
		if (type_size_of(src) == 16) {
			// NOTE(odin-tilde): x != 0 across both limbs
			bool s_swap = cg_u128_is_swapped(src);
			TB_Node *ap = cg_u128_addr_of(p, value);
			TB_Node *lo = cg_emit_u128_limb_load(p, ap, s_swap, 0);
			TB_Node *hi = cg_emit_u128_limb_load(p, ap, s_swap, 1);
			TB_Node *any = tb_inst_or(p->func, lo, hi);
			TB_Node *v = tb_inst_cmp_ne(p->func, any, tb_inst_uint(p->func, TB_TYPE_I64, 0));
			return cg_value(tb_inst_zxt(p->func, v, dt), t);
		}
		TB_Node *v = tb_inst_cmp_ne(p->func, value.node, tb_inst_uint(p->func, st, 0));
		return cg_value(tb_inst_zxt(p->func, v, dt), t);
	}

	if (is_type_cstring(src) && is_type_u8_ptr(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_u8_ptr(src) && is_type_cstring(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_cstring(src) && is_type_u8_multi_ptr(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_u8_multi_ptr(src) && is_type_cstring(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_cstring(src) && is_type_rawptr(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_rawptr(src) && is_type_cstring(dst)) {
		return cg_emit_transmute(p, value, dst);
	}

	if (is_type_cstring16(src) && is_type_u16_ptr(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_u16_ptr(src) && is_type_cstring16(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_cstring16(src) && is_type_u16_multi_ptr(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_u16_multi_ptr(src) && is_type_cstring16(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_cstring16(src) && is_type_rawptr(dst)) {
		return cg_emit_transmute(p, value, dst);
	}
	if (is_type_rawptr(src) && is_type_cstring16(dst)) {
		return cg_emit_transmute(p, value, dst);
	}


	if (are_types_identical(src, t_cstring) && are_types_identical(dst, t_string)) {
		TEMPORARY_ALLOCATOR_GUARD();
		cgValue c = cg_emit_conv(p, value, t_cstring);
		auto args = slice_make<cgValue>(temporary_allocator(), 1);
		args[0] = c;
		cgValue s = cg_emit_runtime_call(p, "cstring_to_string", args);
		return cg_emit_conv(p, s, dst);
	}

	if (are_types_identical(src, t_cstring16) && are_types_identical(dst, t_string16)) {
		TEMPORARY_ALLOCATOR_GUARD();
		cgValue c = cg_emit_conv(p, value, t_cstring16);
		auto args = slice_make<cgValue>(temporary_allocator(), 1);
		args[0] = c;
		cgValue s = cg_emit_runtime_call(p, "cstring16_to_string16", args);
		return cg_emit_conv(p, s, dst);
	}

	// float -> float
	if (is_type_float(src) && is_type_float(dst)) {
		i64 sz = type_size_of(src);
		i64 dz = type_size_of(dst);

		if (sz == 2 || dz == 2) {
			// NOTE(odin-tilde): TB has no f16; half floats are I16 bit patterns,
			// convert in software with f32 as the intermediate step. Non-native
			// endianness halves (e.g. f16be on little-endian) are normalized
			// with a 16-bit byte swap of the bit pattern.
			bool src_swapped = is_type_different_to_arch_endianness(src);
			bool dst_swapped = is_type_different_to_arch_endianness(dst);
			TB_Node *bits = value.node;
			if (sz == 2 && src_swapped) {
				bits = tb_inst_bswap(p->func, bits);
			}
			if (sz == 2 && dz == 2) {
				if (src_swapped != dst_swapped && !src_swapped) {
					bits = tb_inst_bswap(p->func, bits);
				}
				return cg_value(bits, t);
			} else if (sz == 2) {
				TB_Node *f32 = cg_tb_f16_to_f32_node(p->func, bits);
				if (dz == 4) {
					return cg_value(f32, t);
				}
				return cg_value(tb_inst_fpxt(p->func, f32, dt), t);
			} else {
				TB_Node *f32 = value.node;
				if (sz == 8) {
					f32 = tb_inst_fptrunc(p->func, f32, TB_TYPE_F32);
				}
				TB_Node *h = cg_tb_f32_to_f16_node(p->func, f32);
				if (dst_swapped) {
					h = tb_inst_bswap(p->func, h);
				}
				return cg_value(h, t);
			}
		}


		if (dz == sz) {
			if (types_have_same_internal_endian(src, dst)) {
				return cg_value(value.node, t);
			} else {
				return cg_emit_byte_swap(p, value, t);
			}
		}

		if (is_type_different_to_arch_endianness(src) || is_type_different_to_arch_endianness(dst)) {
			Type *platform_src_type = integer_endian_type_to_platform_type(src);
			Type *platform_dst_type = integer_endian_type_to_platform_type(dst);
			cgValue res = {};
			res = cg_emit_conv(p, value, platform_src_type);
			res = cg_emit_conv(p, res, platform_dst_type);
			if (is_type_different_to_arch_endianness(dst)) {
				res = cg_emit_byte_swap(p, res, t);
			}
			return cg_emit_conv(p, res, t);
		}


		if (dz >= sz) {
			return cg_value(tb_inst_fpxt(p->func, value.node, dt), t);
		}
		// PATCH(odin-tilde): float narrowing must be a real value conversion
		// (cvtsd2ss), not an integer-style bit truncation — tb_inst_trunc on an
		// F64 keeps the low 32 bits, which is 0 for values like 1.5.
		return cg_value(tb_inst_fptrunc(p->func, value.node, dt), t);
	}

	if (is_type_complex(src) && is_type_complex(dst)) {
		Type *ft = base_complex_elem_type(dst);
		cgValue ptr = cg_address_from_load_or_generate_local(p, value);
		cgValue re = cg_emit_conv(p, cg_emit_load(p, cg_emit_struct_ep(p, ptr, 0)), ft);
		cgValue im = cg_emit_conv(p, cg_emit_load(p, cg_emit_struct_ep(p, ptr, 1)), ft);
		cgAddr res = cg_add_local(p, t, nullptr, true);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 0), re);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 1), im);
		return cg_addr_load(p, res);
	}

	if (is_type_quaternion(src) && is_type_quaternion(dst)) {
		// @QuaternionLayout
		Type *ft = base_complex_elem_type(dst);
		cgValue ptr = cg_address_from_load_or_generate_local(p, value);
		cgAddr res = cg_add_local(p, t, nullptr, true);
		for (isize i = 0; i < 4; i++) {
			cgValue e = cg_emit_conv(p, cg_emit_load(p, cg_emit_struct_ep(p, ptr, i)), ft);
			cg_emit_store(p, cg_emit_struct_ep(p, res.addr, i), e);
		}
		return cg_addr_load(p, res);
	}
	if (is_type_integer(src) && is_type_complex(dst)) {
		Type *ft = base_complex_elem_type(dst);
		cgValue re = cg_emit_conv(p, value, ft);
		cgAddr res = cg_add_local(p, t, nullptr, true);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 0), re);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 1), cg_const_nil(p, ft));
		return cg_addr_load(p, res);
	}
	if (is_type_float(src) && is_type_complex(dst)) {
		Type *ft = base_complex_elem_type(dst);
		cgValue re = cg_emit_conv(p, value, ft);
		cgAddr res = cg_add_local(p, t, nullptr, true);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 0), re);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 1), cg_const_nil(p, ft));
		return cg_addr_load(p, res);
	}
	if (is_type_integer(src) && is_type_quaternion(dst)) {
		// @QuaternionLayout
		Type *ft = base_complex_elem_type(dst);
		cgValue re = cg_emit_conv(p, value, ft);
		cgValue zero = cg_const_nil(p, ft);
		cgAddr res = cg_add_local(p, t, nullptr, true);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 0), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 1), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 2), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 3), re);
		return cg_addr_load(p, res);
	}
	if (is_type_float(src) && is_type_quaternion(dst)) {
		// @QuaternionLayout
		Type *ft = base_complex_elem_type(dst);
		cgValue re = cg_emit_conv(p, value, ft);
		cgValue zero = cg_const_nil(p, ft);
		cgAddr res = cg_add_local(p, t, nullptr, true);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 0), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 1), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 2), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 3), re);
		return cg_addr_load(p, res);
	}
	if (is_type_complex(src) && is_type_quaternion(dst)) {
		// @QuaternionLayout
		Type *ft = base_complex_elem_type(dst);
		cgValue ptr = cg_address_from_load_or_generate_local(p, value);
		cgValue re = cg_emit_conv(p, cg_emit_load(p, cg_emit_struct_ep(p, ptr, 0)), ft);
		cgValue im = cg_emit_conv(p, cg_emit_load(p, cg_emit_struct_ep(p, ptr, 1)), ft);
		cgValue zero = cg_const_nil(p, ft);
		cgAddr res = cg_add_local(p, t, nullptr, true);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 0), im);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 1), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 2), zero);
		cg_emit_store(p, cg_emit_struct_ep(p, res.addr, 3), re);
		return cg_addr_load(p, res);
	}
	if (is_type_quaternion(src) && is_type_complex(dst)) {
		GB_PANIC("TODO(bill): quaternion -> complex");
	}


	// float <-> integer
	if (is_type_float(src) && is_type_integer(dst)) {
		if (is_type_different_to_arch_endianness(src) || is_type_different_to_arch_endianness(dst)) {
			Type *platform_src_type = integer_endian_type_to_platform_type(src);
			Type *platform_dst_type = integer_endian_type_to_platform_type(dst);
			cgValue res = {};
			res = cg_emit_conv(p, value, platform_src_type);
			res = cg_emit_conv(p, res, platform_dst_type);
			return cg_emit_conv(p, res, t);
		}

		if (cg_is_type_integer_128(dst)) {
			// NOTE(odin-tilde): mirror lb — TB has no i128, go through the
			// runtime soft helpers (fixunsdfdi / fixunsdfti in
			// base/runtime/internal_i128.odin), themselves compiled by this
			// backend on top of the u64 limb primitives.
			char const *call = "fixunsdfdi";
			if (is_type_unsigned(dst)) {
				call = "fixunsdfti";
			}
			auto args = slice_make<cgValue>(permanent_allocator(), 1);
			args[0] = value;
			cgValue res_i128 = cg_emit_runtime_call(p, call, args);
			return cg_emit_conv(p, res_i128, t);
		}

		bool is_signed = !is_type_unsigned(dst);
		if (type_size_of(src) == 2) {
			// NOTE(odin-tilde): f16 is an I16 bit pattern, widen to f32 first
			TB_Node *f32 = cg_tb_f16_to_f32_node(p->func, value.node);
			return cg_value(tb_inst_float2int(p->func, f32, dt, is_signed), t);
		}
		return cg_value(tb_inst_float2int(p->func, value.node, dt, is_signed), t);
	}
	if (is_type_integer(src) && is_type_float(dst)) {
		if (is_type_different_to_arch_endianness(src) || is_type_different_to_arch_endianness(dst)) {
			Type *platform_src_type = integer_endian_type_to_platform_type(src);
			Type *platform_dst_type = integer_endian_type_to_platform_type(dst);
			cgValue res = {};
			res = cg_emit_conv(p, value, platform_src_type);
			res = cg_emit_conv(p, res, platform_dst_type);
			if (is_type_different_to_arch_endianness(dst)) {
				res = cg_emit_byte_swap(p, res, t);
			}
			return cg_emit_conv(p, res, t);
		}

		if (cg_is_type_integer_128(src)) {
			// NOTE(odin-tilde): mirror lb — TB has no i128, go through the
			// runtime soft helpers (floattidf / floattidf_unsigned in
			// base/runtime/internal_i128.odin). json.unmarshal hits this via
			// assign_float(v, i128_from_parse_i128).
			bool is_unsigned = is_type_unsigned(src);
			auto args = slice_make<cgValue>(permanent_allocator(), 1);
			args[0] = cg_emit_conv(p, value, is_unsigned ? t_u128 : t_i128);
			cgValue res_f64 = cg_emit_runtime_call(p, is_unsigned ? "floattidf_unsigned" : "floattidf", args);
			return cg_emit_conv(p, res_f64, t);
		}

		bool is_signed = !is_type_unsigned(dst);
		if (type_size_of(dst) == 2) {
			// NOTE(odin-tilde): f16 is an I16 bit pattern, convert via f32
			TB_Node *f32 = tb_inst_int2float(p->func, value.node, TB_TYPE_F32, is_signed);
			return cg_value(cg_tb_f32_to_f16_node(p->func, f32), t);
		}
		return cg_value(tb_inst_int2float(p->func, value.node, dt, is_signed), t);
	}

	if (is_type_simd_vector(dst)) {
		// PATCH(odin-tilde): scalarized splat — #simd values live in memory,
		// so a scalar -> vector conversion replicates the scalar into every
		// lane of a temporary local
		if (is_type_simd_vector(src)) {
			// PATCH(odin-tilde): vector -> vector (mirrors lb_emit_conv):
			// lane counts must match; identical element types are a no-op,
			// otherwise each lane goes through the scalar cg_emit_conv
			// (IntCast2/FPCast/FPToUI equivalents). A pure bitcast is only
			// valid for identical element types — same-width int<->float is
			// a numeric conversion, not a bitcast.
			Type *src_bt = base_type(src);
			Type *dst_bt = base_type(dst);
			Type *src_et = src_bt->SimdVector.elem;
			Type *dst_et = dst_bt->SimdVector.elem;
			i64 count = dst_bt->SimdVector.count;
			GB_ASSERT_MSG(src_bt->SimdVector.count == count,
			              "%s -> %s (in %.*s)", type_to_string(src), type_to_string(dst), LIT(p->name));
			if (are_types_identical(src_et, dst_et)) {
				value.type = dst;
				return value;
			}
			cgValue sa = cg_address_from_load_or_generate_local(p, value);
			cgValue sp = cg_value(sa.node, alloc_type_pointer(src_et));
			cgAddr tmp = cg_add_local(p, dst, nullptr, false);
			cgValue dp = cg_value(cg_addr_get_ptr(p, tmp).node, alloc_type_pointer(dst_et));
			for (i64 i = 0; i < count; i++) {
				cgValue idx = cg_const_int(p, t_int, i);
				cgValue a = cg_emit_load(p, cg_emit_ptr_offset(p, sp, idx));
				cgValue b = cg_emit_conv(p, a, dst_et);
				cg_emit_store(p, cg_emit_ptr_offset(p, dp, idx), b);
			}
			return cg_addr_load(p, tmp);
		}
		GB_ASSERT_MSG(!is_type_simd_vector(src), "%s -> #simd vector (in %.*s)", type_to_string(src), LIT(p->name));
		Type *bt = base_type(dst);
		Type *et = bt->SimdVector.elem;
		i64 count = bt->SimdVector.count;
		cgValue ev = cg_emit_conv(p, value, et);
		ev = cg_flatten_value(p, ev);
		GB_ASSERT(ev.kind == cgValue_Value);
		cgAddr tmp = cg_add_local(p, dst, nullptr, true);
		cgValue base = cg_value(cg_addr_get_ptr(p, tmp).node, alloc_type_pointer(et));
		for (i64 i = 0; i < count; i++) {
			cgValue ep = cg_emit_ptr_offset(p, base, cg_const_int(p, t_int, i));
			cg_emit_store(p, ep, ev);
		}
		return cg_addr_load(p, tmp);
	}


	// Pointer <-> uintptr
	if (is_type_pointer(src) && is_type_uintptr(dst)) {
		return cg_value(tb_inst_ptr2int(p->func, value.node, dt), t);
	}
	if (is_type_uintptr(src) && is_type_pointer(dst)) {
		return cg_value(tb_inst_int2ptr(p->func, value.node), t);
	}
	if (is_type_multi_pointer(src) && is_type_uintptr(dst)) {
		return cg_value(tb_inst_ptr2int(p->func, value.node, dt), t);
	}
	if (is_type_uintptr(src) && is_type_multi_pointer(dst)) {
		return cg_value(tb_inst_int2ptr(p->func, value.node), t);
	}

	if (is_type_union(dst)) {
		// PATCH(odin-tilde): port of lb_emit_conv's ? -> union handling
		if (dst->Union.variants.count == 1) {
			Type *vt = dst->Union.variants[0];
			if (internal_check_is_assignable_to(src_type, vt)) {
				value = cg_emit_conv(p, value, vt);
				cgAddr parent = cg_add_local(p, t, nullptr, true);
				cg_emit_store_union_variant(p, parent.addr, value, vt);
				return cg_addr_load(p, parent);
			}
		}
		for (Type *vt : dst->Union.variants) {
			if (src_type == t_llvm_bool && is_type_boolean(vt)) {
				cgAddr parent = cg_add_local(p, t, nullptr, true);
				cg_emit_store_union_variant(p, parent.addr, value, vt);
				return cg_addr_load(p, parent);
			}
			if (are_types_identical(src_type, vt)) {
				value = cg_emit_conv(p, value, vt);
				cgAddr parent = cg_add_local(p, t, nullptr, true);
				cg_emit_store_union_variant(p, parent.addr, value, vt);
				return cg_addr_load(p, parent);
			}
		}
		// PATCH(odin-tilde): port of lb_emit_conv's assignability fallback —
		// needed when the source is assignable to a variant but not
		// structurally identical (e.g. bare proc values into proc variants).
		TEMPORARY_ALLOCATOR_GUARD();
		ValidIndexAndScore *valids = gb_alloc_array(temporary_allocator(), ValidIndexAndScore, dst->Union.variants.count);
		isize valid_count = 0;
		isize first_success_index = -1;
		for_array(i, dst->Union.variants) {
			Type *vt = dst->Union.variants[i];
			i64 score = 0;
			if (internal_check_is_assignable_to(src_type, vt)) {
				valids[valid_count].index = i;
				valids[valid_count].score = score;
				valid_count += 1;
				if (first_success_index < 0) {
					first_success_index = i;
				}
			}
		}
		if (valid_count > 1) {
			gb_sort_array(valids, valid_count, valid_index_and_score_cmp);
			i64 best_score = valids[0].score;
			for (isize i = 1; i < valid_count; i++) {
				auto v = valids[i];
				if (best_score > v.score) {
					valid_count = i;
					break;
				}
				best_score = v.score;
			}
			first_success_index = valids[0].index;
		}

		if (valid_count == 1) {
			Type *vt = dst->Union.variants[first_success_index];
			value = cg_emit_conv(p, value, vt);
			cgAddr parent = cg_add_local(p, t, nullptr, true);
			cg_emit_store_union_variant(p, parent.addr, value, vt);
			return cg_addr_load(p, parent);
		}

		GB_PANIC("TODO(bill): %s -> union(%s)", type_to_string(src_type), type_to_string(t));
	}

	// NOTE(bill): This has to be done before 'Pointer <-> Pointer' as it's
	// subtype polymorphism casting
	if (check_is_assignable_to_using_subtype(src_type, t)) {
		Type *st = type_deref(src_type);
		st = type_deref(st);

		bool st_is_ptr = is_type_pointer(src_type);
		st = base_type(st);

		Type *dt = t;

		TEMPORARY_ALLOCATOR_GUARD();

		GB_ASSERT(is_type_struct(st) || is_type_raw_union(st));
		Selection sel = {};
		sel.index.allocator = temporary_allocator();
		if (lookup_subtype_polymorphic_selection(t, src_type, &sel)) {
			if (sel.entity == nullptr) {
				GB_PANIC("invalid subtype cast  %s -> %s", type_to_string(src_type), type_to_string(t));
			}
			if (st_is_ptr) {
				cgValue res = cg_emit_deep_field_gep(p, value, sel);
				Type *rt = res.type;
				if (!are_types_identical(rt, dt) && are_types_identical(type_deref(rt), dt)) {
					res = cg_emit_load(p, res);
				}
				return res;
			} else {
				if (is_type_pointer(value.type)) {
					Type *rt = value.type;
					if (!are_types_identical(rt, dt) && are_types_identical(type_deref(rt), dt)) {
						value = cg_emit_load(p, value);
					} else {
						value = cg_emit_deep_field_gep(p, value, sel);
						return cg_emit_load(p, value);
					}
				}

				cgValue ptr = cg_address_from_load_or_generate_local(p, value);
				cgValue res = cg_emit_deep_field_gep(p, ptr, sel);
				return cg_emit_load(p, res);
			}
		}
	}

	// Pointer <-> Pointer
	if (is_type_pointer(src) && is_type_pointer(dst)) {
		return cg_value(value.node, t);
	}
	if (is_type_multi_pointer(src) && is_type_pointer(dst)) {
		return cg_value(value.node, t);
	}
	if (is_type_pointer(src) && is_type_multi_pointer(dst)) {
		return cg_value(value.node, t);
	}
	if (is_type_multi_pointer(src) && is_type_multi_pointer(dst)) {
		return cg_value(value.node, t);
	}

	// proc <-> proc
	if (is_type_proc(src) && is_type_proc(dst)) {
		return cg_value(value.node, t);
	}

	// pointer -> proc
	if (is_type_pointer(src) && is_type_proc(dst)) {
		return cg_value(value.node, t);
	}
	// proc -> pointer
	if (is_type_proc(src) && is_type_pointer(dst)) {
		return cg_value(value.node, t);
	}

	// []byte/[]u8 <-> string
	if (is_type_u8_slice(src) && is_type_string(dst)) {
		return cg_emit_transmute(p, value, t);
	}
	if (is_type_string(src) && is_type_u8_slice(dst)) {
		return cg_emit_transmute(p, value, t);
	}

	// []u16 <-> string16
	if (is_type_u16_slice(src) && is_type_string16(dst)) {
		return cg_emit_transmute(p, value, t);
	}
	if (is_type_string16(src) && is_type_u16_slice(dst)) {
		return cg_emit_transmute(p, value, t);
	}

	if (is_type_matrix(dst) && !is_type_matrix(src)) {
		// NOTE(odin-tilde): mirrors lb — a scalar converts to a square
		// matrix as a scaled identity matrix.
		GB_ASSERT_MSG(dst->Matrix.row_count == dst->Matrix.column_count, "%s <- %s", type_to_string(dst), type_to_string(src));

		Type *elem = base_array_type(dst);
		cgValue e = cg_emit_conv(p, value, elem);
		cgAddr v = cg_add_local(p, t, nullptr, false);
		// matrix elems may be int, float or complex — build the zero
		// constant with the matching ExactValue kind.
		cgValue zero = {};
		if (is_type_float(elem)) {
			zero = cg_const_value(p, elem, exact_value_float(0));
		} else if (is_type_complex(elem)) {
			ExactValue cz = {};
			cz.kind = ExactValue_Complex;
			zero = cg_const_value(p, elem, cz);
		} else {
			zero = cg_const_value(p, elem, exact_value_i64(0));
		}
		for (i64 j = 0; j < dst->Matrix.column_count; j++) {
			for (i64 i = 0; i < dst->Matrix.row_count; i++) {
				cgValue ptr = cg_emit_matrix_epi(p, v.addr, i, j);
				cg_emit_store(p, ptr, i == j ? e : zero);
			}
		}

		return cg_addr_load(p, v);
	}

	if (is_type_matrix(dst) && is_type_matrix(src)) {
		// NOTE(odin-tilde): mirrors lb's matrix->matrix conversion.
		GB_ASSERT(dst->kind == Type_Matrix);
		GB_ASSERT(src->kind == Type_Matrix);
		cgAddr v = cg_add_local(p, t, nullptr, true);
		cgValue psrc = cg_address_from_load_or_generate_local(p, value);

		if (dst->Matrix.row_count    == src->Matrix.row_count &&
		    dst->Matrix.column_count == src->Matrix.column_count) {
			for (i64 j = 0; j < dst->Matrix.column_count; j++) {
				for (i64 i = 0; i < dst->Matrix.row_count; i++) {
					cgValue d = cg_emit_matrix_epi(p, v.addr, i, j);
					cgValue s = cg_emit_load(p, cg_emit_matrix_epi(p, psrc, i, j));
					s = cg_emit_conv(p, s, dst->Matrix.elem);
					cg_emit_store(p, d, s);
				}
			}
		} else if (is_matrix_square(dst) && is_matrix_square(src)) {
			for (i64 j = 0; j < dst->Matrix.column_count; j++) {
				for (i64 i = 0; i < dst->Matrix.row_count; i++) {
					if (i < src->Matrix.row_count && j < src->Matrix.column_count) {
						cgValue d = cg_emit_matrix_epi(p, v.addr, i, j);
						cgValue s = cg_emit_load(p, cg_emit_matrix_epi(p, psrc, i, j));
						s = cg_emit_conv(p, s, dst->Matrix.elem);
						cg_emit_store(p, d, s);
					} else if (i == j) {
						cgValue d = cg_emit_matrix_epi(p, v.addr, i, j);
						cgValue one = {};
						if (is_type_float(dst->Matrix.elem)) {
							one = cg_const_value(p, dst->Matrix.elem, exact_value_float(1));
						} else if (is_type_complex(dst->Matrix.elem)) {
							ExactValue co = {};
							co.kind = ExactValue_Complex;
							co.value_complex = permanent_alloc_item<Complex128>();
							co.value_complex->real = 1;
							one = cg_const_value(p, dst->Matrix.elem, co);
						} else {
							one = cg_const_value(p, dst->Matrix.elem, exact_value_i64(1));
						}
						cg_emit_store(p, d, one);
					}
				}
			}
		} else {
			i64 dst_count = dst->Matrix.row_count*dst->Matrix.column_count;
			i64 src_count = src->Matrix.row_count*src->Matrix.column_count;
			GB_ASSERT(dst_count == src_count);

			bool same_elem_base_types = are_types_identical(
				base_type(dst->Matrix.elem),
				base_type(src->Matrix.elem)
			);

			if (same_elem_base_types && type_size_of(dst) == type_size_of(src)) {
				i64 size = type_size_of(dst);
				tb_inst_memcpy(p->func, v.addr.node, psrc.node,
				               tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)size),
				               cast(TB_CharUnits)type_align_of(dst));
			} else {
				for (i64 i = 0; i < src_count; i++) {
					cgValue dp = cg_emit_array_epi(p, v.addr, matrix_column_major_index_to_offset(dst, i));
					cgValue sp = cg_emit_array_epi(p, psrc,   matrix_column_major_index_to_offset(src, i));
					cgValue s = cg_emit_load(p, sp);
					s = cg_emit_conv(p, s, dst->Matrix.elem);
					cg_emit_store(p, dp, s);
				}
			}
		}
		return cg_addr_load(p, v);
	}

	if (is_type_any(dst)) {
		if (is_type_untyped_nil(src) ||
		    is_type_untyped_uninit(src)) {
			return cg_const_nil(p, t);
		}

		cgAddr result = cg_add_local(p, t, nullptr, false);

		Type *st = default_type(src_type);

		cgValue data = cg_address_from_load_or_generate_local(p, value);
		GB_ASSERT(is_type_pointer(data.type));
		GB_ASSERT(is_type_typed(st));

		data = cg_emit_conv(p, data, t_rawptr);

		cgValue id = cg_typeid(p, st);
		cgValue data_ptr = cg_emit_struct_ep(p, result.addr, 0);
		cgValue id_ptr   = cg_emit_struct_ep(p, result.addr, 1);

		cg_emit_store(p, data_ptr, data);
		cg_emit_store(p, id_ptr,   id);

		return cg_addr_load(p, result);
	}

	i64 src_sz = type_size_of(src);
	i64 dst_sz = type_size_of(dst);

	if (src_sz == dst_sz) {
		// bit_set <-> integer
		if (is_type_integer(src) && is_type_bit_set(dst)) {
			cgValue v = cg_emit_conv(p, value, bit_set_to_int(dst));
			return cg_emit_transmute(p, v, t);
		}
		if (is_type_bit_set(src) && is_type_integer(dst)) {
			cgValue bs = cg_emit_transmute(p, value, bit_set_to_int(src));
			return cg_emit_conv(p, bs, dst);
		}

		// typeid <-> integer
		if (is_type_integer(src) && is_type_typeid(dst)) {
			return cg_emit_transmute(p, value, dst);
		}
		if (is_type_typeid(src) && is_type_integer(dst)) {
			return cg_emit_transmute(p, value, dst);
		}
	}


	if (is_type_untyped(src)) {
		if (is_type_string(src) && is_type_string(dst)) {
			cgAddr result = cg_add_local(p, t, nullptr, false);
			cg_addr_store(p, result, value);
			return cg_addr_load(p, result);
		}
	}


	gb_printf_err("%.*s\n", LIT(p->name));
	gb_printf_err("cg_emit_conv: src -> dst\n");
	gb_printf_err("Not Identical %s != %s\n", type_to_string(src_type), type_to_string(t));
	gb_printf_err("Not Identical %s != %s\n", type_to_string(src), type_to_string(dst));
	gb_printf_err("Not Identical %p != %p\n", src_type, t);
	gb_printf_err("Not Identical %p != %p\n", src, dst);

	GB_PANIC("Invalid type conversion: '%s' to '%s' for procedure '%.*s'",
	         type_to_string(src_type), type_to_string(t),
	         LIT(p->name));

	return {};
}

gb_internal cgValue cg_emit_arith(cgProcedure *p, TokenKind op, cgValue lhs, cgValue rhs, Type *type) {
	if (is_type_array_like(lhs.type) || is_type_array_like(rhs.type)) {
		// NOTE(odin-tilde): element-wise array arithmetic. A scalar operand
		// broadcasts across the elements (LLVM splats it into the array via
		// conv; here the scalar is simply reused for every element).
		GB_ASSERT(is_type_array_like(type));
		Type *elem_type = base_array_type(type);
		i64 count = get_array_type_count(type);

		cgValue x = lhs, y = rhs;
		cgValue x_ptr = {}, y_ptr = {};
		if (is_type_array_like(x.type)) {
			x = cg_emit_conv(p, x, type);
			x_ptr = cg_address_from_load_or_generate_local(p, x);
		} else {
			x = cg_flatten_value(p, cg_emit_conv(p, x, elem_type));
		}
		if (is_type_array_like(y.type)) {
			y = cg_emit_conv(p, y, type);
			y_ptr = cg_address_from_load_or_generate_local(p, y);
		} else {
			y = cg_flatten_value(p, cg_emit_conv(p, y, elem_type));
		}

		cgAddr res = cg_add_local(p, type, nullptr, false);
		cgValue dst_ptr = cg_addr_get_ptr(p, res);
		for (i64 i = 0; i < count; i++) {
			cgValue a = x_ptr.node ? cg_emit_load(p, cg_emit_array_epi(p, x_ptr, i)) : x;
			cgValue b = y_ptr.node ? cg_emit_load(p, cg_emit_array_epi(p, y_ptr, i)) : y;
			cgValue c = cg_emit_arith(p, op, a, b, elem_type);
			cg_emit_store(p, cg_emit_array_epi(p, dst_ptr, i), c);
		}
		return cg_addr_load(p, res);
	} else if (is_type_matrix(lhs.type) || is_type_matrix(rhs.type)) {
		return cg_emit_arith_matrix(p, op, lhs, rhs, type, false);
	} else if (is_type_complex(type)) {
		// NOTE(odin-tilde): mirrors lb_emit_arith's complex path. complex is
		// a 2-field memory aggregate here; division goes through the runtime
		// (Smith's algorithm), everything else is element-wise. f16 elements
		// are promoted to f32 for the arithmetic itself since TB has no f16.
		lhs = cg_emit_conv(p, lhs, type);
		rhs = cg_emit_conv(p, rhs, type);

		Type *ft = base_complex_elem_type(type);

		if (op == Token_Quo) {
			auto args = slice_make<cgValue>(permanent_allocator(), 2);
			args[0] = lhs;
			args[1] = rhs;
			switch (type_size_of(ft)) {
			case 2: return cg_emit_runtime_call(p, "quo_complex32", args);
			case 4: return cg_emit_runtime_call(p, "quo_complex64", args);
			case 8: return cg_emit_runtime_call(p, "quo_complex128", args);
			default: GB_PANIC("Unknown float type"); break;
			}
		}

		Type *it = ft;
		if (type_size_of(ft) == 2) {
			it = t_f32;
		}

		cgValue lptr = cg_address_from_load_or_generate_local(p, lhs);
		cgValue rptr = cg_address_from_load_or_generate_local(p, rhs);
		cgValue a = cg_emit_load(p, cg_emit_struct_ep(p, lptr, 0));
		cgValue b = cg_emit_load(p, cg_emit_struct_ep(p, lptr, 1));
		cgValue c = cg_emit_load(p, cg_emit_struct_ep(p, rptr, 0));
		cgValue d = cg_emit_load(p, cg_emit_struct_ep(p, rptr, 1));
		if (it != ft) {
			a = cg_emit_conv(p, a, it);
			b = cg_emit_conv(p, b, it);
			c = cg_emit_conv(p, c, it);
			d = cg_emit_conv(p, d, it);
		}

		cgValue real = {};
		cgValue imag = {};
		switch (op) {
		case Token_Add:
		case Token_Sub:
			real = cg_emit_arith(p, op, a, c, it);
			imag = cg_emit_arith(p, op, b, d, it);
			break;
		case Token_Mul: {
			cgValue x = cg_emit_arith(p, Token_Mul, a, c, it);
			cgValue y = cg_emit_arith(p, Token_Mul, b, d, it);
			real = cg_emit_arith(p, Token_Sub, x, y, it);
			cgValue z = cg_emit_arith(p, Token_Mul, b, c, it);
			cgValue w = cg_emit_arith(p, Token_Mul, a, d, it);
			imag = cg_emit_arith(p, Token_Add, z, w, it);
			break;
		}
		default:
			GB_PANIC("cg_emit_arith complex: invalid op");
			break;
		}
		if (it != ft) {
			real = cg_emit_conv(p, real, ft);
			imag = cg_emit_conv(p, imag, ft);
		}

		cgAddr res = cg_add_local(p, type, nullptr, false);
		cgValue dst = cg_addr_get_ptr(p, res);
		cg_emit_store(p, cg_emit_struct_ep(p, dst, 0), real);
		cg_emit_store(p, cg_emit_struct_ep(p, dst, 1), imag);
		return cg_addr_load(p, res);
	} else if (is_type_quaternion(type)) {
		// NOTE(odin-tilde): mirrors lb_emit_arith's quaternion path.
		// @QuaternionLayout: i,j,k,w at indices 0,1,2,3. mul/div go through
		// the runtime; add/sub are element-wise (f16 promoted to f32).
		lhs = cg_emit_conv(p, lhs, type);
		rhs = cg_emit_conv(p, rhs, type);

		Type *ft = base_complex_elem_type(type);

		if (op == Token_Mul || op == Token_Quo) {
			char const *name = nullptr;
			switch (8*type_size_of(ft)) {
			case 16: name = (op == Token_Mul) ? "mul_quaternion64"  : "quo_quaternion64";  break;
			case 32: name = (op == Token_Mul) ? "mul_quaternion128" : "quo_quaternion128"; break;
			case 64: name = (op == Token_Mul) ? "mul_quaternion256" : "quo_quaternion256"; break;
			default: GB_PANIC("Unknown float type"); break;
			}
			auto args = slice_make<cgValue>(permanent_allocator(), 2);
			args[0] = lhs;
			args[1] = rhs;
			return cg_emit_runtime_call(p, name, args);
		}

		GB_ASSERT(op == Token_Add || op == Token_Sub);

		Type *it = ft;
		if (type_size_of(ft) == 2) {
			it = t_f32;
		}

		cgValue lptr = cg_address_from_load_or_generate_local(p, lhs);
		cgValue rptr = cg_address_from_load_or_generate_local(p, rhs);
		cgAddr res = cg_add_local(p, type, nullptr, false);
		cgValue dst = cg_addr_get_ptr(p, res);
		for (isize i = 0; i < 4; i++) {
			cgValue x = cg_emit_load(p, cg_emit_struct_ep(p, lptr, i));
			cgValue y = cg_emit_load(p, cg_emit_struct_ep(p, rptr, i));
			if (it != ft) {
				x = cg_emit_conv(p, x, it);
				y = cg_emit_conv(p, y, it);
			}
			cgValue z = cg_emit_arith(p, op, x, y, it);
			if (it != ft) {
				z = cg_emit_conv(p, z, ft);
			}
			cg_emit_store(p, cg_emit_struct_ep(p, dst, i), z);
		}
		return cg_addr_load(p, res);
	}

	if (cg_is_type_integer_128(type)) {
		// NOTE(odin-tilde): 128-bit integers are memory aggregates here
		return cg_emit_arith_u128(p, op, lhs, rhs, type);
	}

	if (is_type_simd_vector(type)) {
		// NOTE(odin-tilde): #simd vectors are memory aggregates; per-lane
		// scalarization lives in tilde_builtin.cpp with the simd intrinsics
		return cg_emit_arith_simd(p, op, lhs, rhs, type);
	}

	lhs = cg_flatten_value(p, cg_emit_conv(p, lhs, type));
	rhs = cg_flatten_value(p, cg_emit_conv(p, rhs, type));
	GB_ASSERT(lhs.kind == cgValue_Value);
	GB_ASSERT(rhs.kind == cgValue_Value);

	if (is_type_integer(type) && is_type_different_to_arch_endianness(type)) {
		switch (op) {
		case Token_AndNot:
		case Token_And:
		case Token_Or:
		case Token_Xor:
			goto handle_op;
		}

		Type *platform_type = integer_endian_type_to_platform_type(type);
		cgValue x = cg_emit_byte_swap(p, lhs, integer_endian_type_to_platform_type(lhs.type));
		cgValue y = cg_emit_byte_swap(p, rhs, integer_endian_type_to_platform_type(rhs.type));

		cgValue res = cg_emit_arith(p, op, x, y, platform_type);

		return cg_emit_byte_swap(p, res, type);
	}

	if (is_type_float(type) && is_type_different_to_arch_endianness(type)) {
		Type *platform_type = integer_endian_type_to_platform_type(type);
		cgValue x = cg_emit_conv(p, lhs, integer_endian_type_to_platform_type(lhs.type));
		cgValue y = cg_emit_conv(p, rhs, integer_endian_type_to_platform_type(rhs.type));

		cgValue res = cg_emit_arith(p, op, x, y, platform_type);

		return cg_emit_byte_swap(p, res, type);
	}

handle_op:;

	// NOTE(bill): Bit Set Aliases for + and -
	if (is_type_bit_set(type)) {
		switch (op) {
		case Token_Add: op = Token_Or;     break;
		case Token_Sub: op = Token_AndNot; break;
		}
	}

	// NOTE(odin-tilde): f16 is kept as an I16 bit pattern (TB has no f16
	// float ops). Promote to f32, do the arithmetic there, then round the
	// result back to f16 (round-to-nearest-even) — this matches the single
	// rounding of native f16 arithmetic. Endianness variants are already
	// normalized to platform f16 above, so the bit patterns need no swap.
	if (is_type_float(type) && type_size_of(type) == 2) {
		switch (op) {
		case Token_Add:
		case Token_Sub:
		case Token_Mul:
		case Token_Quo: {
			TB_Node *x = cg_tb_f16_to_f32_node(p->func, lhs.node);
			TB_Node *y = cg_tb_f16_to_f32_node(p->func, rhs.node);
			TB_Node *res = nullptr;
			switch (op) {
			case Token_Add: res = tb_inst_fadd(p->func, x, y); break;
			case Token_Sub: res = tb_inst_fsub(p->func, x, y); break;
			case Token_Mul: res = tb_inst_fmul(p->func, x, y); break;
			case Token_Quo: res = tb_inst_fdiv(p->func, x, y); break;
			}
			return cg_value(cg_tb_f32_to_f16_node(p->func, res), type);
		}
		}
	}

	TB_ArithmeticBehavior arith_behavior = cast(TB_ArithmeticBehavior)0;

	Type *integral_type = type;
	if (is_type_simd_vector(integral_type)) {
		GB_PANIC("TODO(bill): cg_emit_arith #simd vector");
		// integral_type = core_array_type(integral_type);
	}

	switch (op) {
	case Token_Add:
		if (is_type_float(integral_type)) {
			return cg_value(tb_inst_fadd(p->func, lhs.node, rhs.node), type);
		}
		return cg_value(tb_inst_add(p->func, lhs.node, rhs.node, arith_behavior), type);
	case Token_Sub:
		if (is_type_float(integral_type)) {
			return cg_value(tb_inst_fsub(p->func, lhs.node, rhs.node), type);
		}
		return cg_value(tb_inst_sub(p->func, lhs.node, rhs.node, arith_behavior), type);
	case Token_Mul:
		if (is_type_float(integral_type)) {
			return cg_value(tb_inst_fmul(p->func, lhs.node, rhs.node), type);
		}
		return cg_value(tb_inst_mul(p->func, lhs.node, rhs.node, arith_behavior), type);
	case Token_Quo:
		if (is_type_float(integral_type)) {
			return cg_value(tb_inst_fdiv(p->func, lhs.node, rhs.node), type);
		}
		return cg_value(tb_inst_div(p->func, lhs.node, rhs.node, !is_type_unsigned(integral_type)), type);
	case Token_Mod:
		if (is_type_float(integral_type)) {
			GB_PANIC("TODO(bill): float %% float");
		}
		return cg_value(tb_inst_mod(p->func, lhs.node, rhs.node, !is_type_unsigned(integral_type)), type);
	case Token_ModMod:
		if (is_type_unsigned(integral_type)) {
			return cg_value(tb_inst_mod(p->func, lhs.node, rhs.node, false), type);
		} else {
			TB_Node *a = tb_inst_mod(p->func, lhs.node, rhs.node, true);
			TB_Node *b = tb_inst_add(p->func, a, rhs.node, arith_behavior);
			TB_Node *c = tb_inst_mod(p->func, b, rhs.node, true);
			return cg_value(c, type);
		}

	case Token_And:
		return cg_value(tb_inst_and(p->func, lhs.node, rhs.node), type);
	case Token_Or:
		return cg_value(tb_inst_or(p->func, lhs.node, rhs.node), type);
	case Token_Xor:
		return cg_value(tb_inst_xor(p->func, lhs.node, rhs.node), type);
	case Token_Shl:
		{
			rhs = cg_emit_conv(p, rhs, lhs.type);
			TB_DataType dt = cg_data_type(lhs.type);
			TB_Node *lhsval = lhs.node;
			TB_Node *bits = rhs.node;

			TB_Node *bit_size = tb_inst_uint(p->func, dt, 8*type_size_of(lhs.type));
			TB_Node *zero = tb_inst_uint(p->func, dt, 0);

			TB_Node *width_test = tb_inst_cmp_ilt(p->func, bits, bit_size, false);

			TB_Node *res = tb_inst_shl(p->func, lhsval, bits, arith_behavior);
			res = tb_inst_select(p->func, width_test, res, zero);
			return cg_value(res, type);
		}
	case Token_Shr:
		{
			rhs = cg_emit_conv(p, rhs, lhs.type);
			TB_DataType dt = cg_data_type(lhs.type);
			TB_Node *lhsval = lhs.node;
			TB_Node *bits = rhs.node;

			TB_Node *bit_size = tb_inst_uint(p->func, dt, 8*type_size_of(lhs.type));
			TB_Node *zero = tb_inst_uint(p->func, dt, 0);

			TB_Node *width_test = tb_inst_cmp_ilt(p->func, bits, bit_size, false);

			TB_Node *res = nullptr;

			if (is_type_unsigned(integral_type)) {
				res = tb_inst_shr(p->func, lhsval, bits);
			} else {
				res = tb_inst_sar(p->func, lhsval, bits);
			}


			res = tb_inst_select(p->func, width_test, res, zero);
			return cg_value(res, type);
		}
	case Token_AndNot:
		return cg_value(tb_inst_and(p->func, lhs.node, tb_inst_not(p->func, rhs.node)), type);
	}

	GB_PANIC("unhandled operator of cg_emit_arith");

	return {};
}


gb_internal void cg_fill_slice(cgProcedure *p, cgAddr const &slice, cgValue data, cgValue len) {
	cgValue slice_ptr = cg_addr_get_ptr(p, slice);
	cgValue data_ptr = cg_emit_struct_ep(p, slice_ptr, 0);
	cgValue len_ptr  = cg_emit_struct_ep(p, slice_ptr, 1);

	data = cg_emit_conv(p, data, type_deref(data_ptr.type));
	len = cg_emit_conv(p, len, t_int);
	cg_emit_store(p, data_ptr, data);
	cg_emit_store(p, len_ptr,  len);
}

gb_internal cgAddr cg_build_addr_slice_expr(cgProcedure *p, Ast *expr) {
	ast_node(se, SliceExpr, expr);

	cgValue low  = cg_const_int(p, t_int, 0);
	cgValue high = {};

	if (se->low  != nullptr) {
		low = cg_correct_endianness(p, cg_build_expr(p, se->low));
	}
	if (se->high != nullptr) {
		high = cg_correct_endianness(p, cg_build_expr(p, se->high));
	}

	bool no_indices = se->low == nullptr && se->high == nullptr;
	gb_unused(no_indices);

	cgAddr addr = cg_build_addr(p, se->expr);
	cgValue base = cg_addr_load(p, addr);
	Type *type = base_type(base.type);

	if (is_type_pointer(type)) {
		type = base_type(type_deref(type));
		addr = cg_addr(base);
		base = cg_addr_load(p, addr);
	}

	switch (type->kind) {
	case Type_Basic:
	case Type_Slice: {
		if (type->kind == Type_Basic) {
			GB_ASSERT(type->Basic.kind == Basic_string || type->Basic.kind == Basic_string16);
		}

		Type *slice_type = type;
		if (high.node == nullptr) {
			cgValue len = cg_builtin_len(p, base);
			high = len;
		}

		if (!no_indices) {
			// cg_emit_slice_bounds_check(p, se->open, low, high, len, se->low != nullptr);
		}

		cgValue elem    = cg_emit_ptr_offset(p, cg_builtin_raw_data(p, base), low);
		cgValue new_len = cg_emit_arith(p, Token_Sub, high, low, t_int);

		// NOTE(odin-tilde): no zero-init — cg_fill_slice overwrites both fields,
		// and the redundant memset can be scheduled between a later call's
		// argument setup and the call itself (rep stosb clobbers RDI).
		cgAddr slice = cg_add_local(p, slice_type, nullptr, false);
		cg_fill_slice(p, slice, elem, new_len);
		return slice;
	}

	case Type_DynamicArray: {
		// PATCH(odin-tilde): mirror the Type_Slice path; the dynamic array
		// rvalue lives in memory so raw_data/len read its fields directly.
		Type *slice_type = type_of_expr(expr);
		GB_ASSERT(is_type_slice(slice_type));
		cgValue data = cg_builtin_raw_data(p, base);
		cgValue len  = cg_builtin_len(p, base);
		if (high.node == nullptr) {
			high = len;
		}
		cgValue elem    = cg_emit_ptr_offset(p, data, low);
		cgValue new_len = cg_emit_arith(p, Token_Sub, high, low, t_int);
		cgAddr res = cg_add_local(p, slice_type, nullptr, false);
		cg_fill_slice(p, res, elem, new_len);
		return res;
	}

	case Type_MultiPointer: {
		Type *res_type = type_of_expr(expr);
		if (se->high == nullptr) {
			cgAddr res = cg_add_local(p, res_type, nullptr, false);
			GB_ASSERT(base.kind == cgValue_Value);
			GB_ASSERT(low.kind == cgValue_Value);

			i64 stride = type_size_of(type->MultiPointer.elem);
			cgValue offset = cg_value(tb_inst_array_access(p->func, base.node, low.node, stride), base.type);
			cg_addr_store(p, res, offset);
			return res;
		} else {
			cgAddr res = cg_add_local(p, res_type, nullptr, true);
			low  = cg_emit_conv(p, low,  t_int);
			high = cg_emit_conv(p, high, t_int);

			// cg_emit_multi_pointer_slice_bounds_check(p, se->open, low, high);

			i64 stride = type_size_of(type->MultiPointer.elem);
			TB_Node *offset = tb_inst_array_access(p->func, base.node, low.node, stride);
			TB_Node *len = tb_inst_sub(p->func, high.node, low.node, cast(TB_ArithmeticBehavior)0);

			TB_Node *data_ptr = tb_inst_member_access(p->func, res.addr.node, type_offset_of(res_type, 0));
			TB_Node *len_ptr  = tb_inst_member_access(p->func, res.addr.node, type_offset_of(res_type, 1));

			tb_inst_store(p->func, TB_TYPE_PTR, data_ptr, offset, cast(TB_CharUnits)build_context.ptr_size, false);
			tb_inst_store(p->func, TB_TYPE_INT, len_ptr,  len,    cast(TB_CharUnits)build_context.int_size, false);
			return res;
		}
	}

	case Type_Array: {
		Type *slice_type = type_of_expr(expr);
		GB_ASSERT(is_type_slice(slice_type));
		cgValue len = cg_const_int(p, t_int, type->Array.count);
		if (high.node == nullptr) high = len;

		// bool low_const  = type_and_value_of_expr(se->low).mode  == Addressing_Constant;
		// bool high_const = type_and_value_of_expr(se->high).mode == Addressing_Constant;
		// if (!low_const || !high_const) {
		// 	if (!no_indices) {
		// 		lb_emit_slice_bounds_check(p, se->open, low, high, len, se->low != nullptr);
		// 	}
		// }
		cgValue elem    = cg_emit_ptr_offset(p, cg_builtin_raw_data(p, cg_addr_get_ptr(p, addr)), low);
		cgValue new_len = cg_emit_arith(p, Token_Sub, high, low, t_int);

		cgAddr slice = cg_add_local(p, slice_type, nullptr, true);
		cg_fill_slice(p, slice, elem, new_len);
		return slice;
	}

	case Type_FixedCapacityDynamicArray: {
		// NOTE(odin-tilde): mirror the Type_Array path — the data buffer is
		// inline at offset 0, so slicing produces {base + low*stride, high-low}.
		Type *slice_type = type_of_expr(expr);
		GB_ASSERT(is_type_slice(slice_type));
		if (high.node == nullptr) {
			high = cg_builtin_len(p, base);
		}
		cgValue base_ptr = cg_addr_get_ptr(p, addr);
		cgValue elem     = cg_emit_ptr_offset(p, cg_builtin_raw_data(p, base_ptr), low);
		cgValue new_len  = cg_emit_arith(p, Token_Sub, high, low, t_int);

		cgAddr slice = cg_add_local(p, slice_type, nullptr, true);
		cg_fill_slice(p, slice, elem, new_len);
		return slice;
	}


	case Type_Struct:
		if (is_type_soa_struct(type)) {
			// NOTE(odin-tilde): mirrors the lb #soa slicing logic (kept as
			// a comment above this branch in earlier revisions). Slicing a
			// #soa value produces a #soa slice struct: each component field
			// becomes a pointer offset by `low`, plus the new length.
			Type *res_type = type_of_expr(expr);
			cgValue base_ptr = cg_address_from_load_or_generate_local(p, base);

			cgValue len = {};
			if (type->Struct.soa_kind == StructSoa_Fixed) {
				len = cg_const_int(p, t_int, type->Struct.soa_count);
			} else {
				len = cg_builtin_len(p, base);
			}
			if (high.node == nullptr) high = len;

			cgAddr dst = cg_add_local(p, res_type, nullptr, false);
			if (type->Struct.soa_kind == StructSoa_Fixed) {
				i32 field_count = cast(i32)type->Struct.fields.count;
				for (i32 i = 0; i < field_count; i++) {
					cgValue field_dst = cg_emit_struct_ep(p, dst.addr, i);
					cgValue field_src = cg_emit_struct_ep(p, base_ptr, i);
					field_src = cg_emit_array_ep(p, field_src, low);
					// ^elem -> [^]elem (the #soa slice fields are multi-pointers)
					field_src = cg_emit_conv(p, field_src, type_deref(field_dst.type));
					cg_emit_store(p, field_dst, field_src);
				}

				cgValue len_dst = cg_emit_struct_ep(p, dst.addr, field_count);
				cgValue new_len = cg_emit_arith(p, Token_Sub, high, low, t_int);
				cg_emit_store(p, len_dst, new_len);
			} else if (type->Struct.soa_kind == StructSoa_Slice) {
				if (no_indices) {
					cg_addr_store(p, dst, base);
				} else {
					i32 field_count = cast(i32)type->Struct.fields.count - 1;
					for (i32 i = 0; i < field_count; i++) {
						cgValue field_dst = cg_emit_struct_ep(p, dst.addr, i);
						cgValue field_src = cg_emit_load(p, cg_emit_struct_ep(p, base_ptr, i));
						field_src = cg_emit_ptr_offset(p, field_src, low);
						field_src = cg_emit_conv(p, field_src, type_deref(field_dst.type));
						cg_emit_store(p, field_dst, field_src);
					}

					cgValue len_dst = cg_emit_struct_ep(p, dst.addr, field_count);
					cgValue new_len = cg_emit_arith(p, Token_Sub, high, low, t_int);
					cg_emit_store(p, len_dst, new_len);
				}
			} else if (type->Struct.soa_kind == StructSoa_Dynamic) {
				i32 field_count = cast(i32)type->Struct.fields.count - 3;
				for (i32 i = 0; i < field_count; i++) {
					cgValue field_dst = cg_emit_struct_ep(p, dst.addr, i);
					cgValue field_src = cg_emit_load(p, cg_emit_struct_ep(p, base_ptr, i));
					field_src = cg_emit_ptr_offset(p, field_src, low);
					field_src = cg_emit_conv(p, field_src, type_deref(field_dst.type));
					cg_emit_store(p, field_dst, field_src);
				}

				cgValue len_dst = cg_emit_struct_ep(p, dst.addr, field_count);
				cgValue new_len = cg_emit_arith(p, Token_Sub, high, low, t_int);
				cg_emit_store(p, len_dst, new_len);
			}

			return dst;
		}
		GB_PANIC("cg_build_addr_slice_expr Type_Struct");
		break;

	}

	GB_PANIC("Unknown slicable type: %s (kind=%d)", type_to_string(type), type->kind);
	return {};
}

gb_internal cgValue cg_emit_unary_arith(cgProcedure *p, TokenKind op, cgValue x, Type *type) {
	switch (op) {
	case Token_Add:
		return x;
	case Token_Not: // Boolean not
	case Token_Xor: // Bitwise not
	case Token_Sub: // Number negation
		break;
	case Token_Pointer:
		GB_PANIC("This should be handled elsewhere");
		break;
	}

	x = cg_flatten_value(p, x);

	if (cg_is_type_integer_128(x.type)) {
		// NOTE(odin-tilde): 128-bit integers are memory aggregates here
		if (op == Token_Xor) {
			bool swapped = cg_u128_is_swapped(x.type);
			TB_Node *ap = cg_u128_addr_of(p, x);
			TB_Node *lo = tb_inst_not(p->func, cg_emit_u128_limb_load(p, ap, swapped, 0));
			TB_Node *hi = tb_inst_not(p->func, cg_emit_u128_limb_load(p, ap, swapped, 1));
			cgValue res = cg_u128_make_value(p, x.type, swapped, lo, hi);
			return cg_emit_conv(p, res, type);
		}
		if (op == Token_Sub) {
			cgValue zero = cg_const_nil(p, x.type);
			cgValue res = cg_emit_arith_u128(p, Token_Sub, zero, x, x.type);
			return cg_emit_conv(p, res, type);
		}
		GB_PANIC("cg_emit_unary_arith: unhandled 128-bit op");
	}

	if (is_type_array_like(x.type)) {
		GB_PANIC("TODO(bill): cg_emit_unary_arith is_type_array_like");
		// // IMPORTANT TODO(bill): This is very wasteful with regards to stack memory
		// Type *tl = base_type(x.type);
		// cgValue val = cg_address_from_load_or_generate_local(p, x);
		// GB_ASSERT(is_type_array_like(type));
		// Type *elem_type = base_array_type(type);

		// // NOTE(bill): Doesn't need to be zero because it will be initialized in the loops
		// cgAddr res_addr = cg_add_local(p, type, nullptr, false);
		// cgValue res = cg_addr_get_ptr(p, res_addr);

		// bool inline_array_arith = cg_can_try_to_inline_array_arith(type);

		// i32 count = cast(i32)get_array_type_count(tl);

		// LLVMTypeRef vector_type = nullptr;
		// if (op != Token_Not && cg_try_vector_cast(p->module, val, &vector_type)) {
		// 	LLVMValueRef vp = LLVMBuildPointerCast(p->builder, val.value, LLVMPointerType(vector_type, 0), "");
		// 	LLVMValueRef v = LLVMBuildLoad2(p->builder, vector_type, vp, "");

		// 	LLVMValueRef opv = nullptr;
		// 	switch (op) {
		// 	case Token_Xor:
		// 		opv = LLVMBuildNot(p->builder, v, "");
		// 		break;
		// 	case Token_Sub:
		// 		if (is_type_float(elem_type)) {
		// 			opv = LLVMBuildFNeg(p->builder, v, "");
		// 		} else {
		// 			opv = LLVMBuildNeg(p->builder, v, "");
		// 		}
		// 		break;
		// 	}

		// 	if (opv != nullptr) {
		// 		LLVMSetAlignment(res.value, cast(unsigned)cg_alignof(vector_type));
		// 		LLVMValueRef res_ptr = LLVMBuildPointerCast(p->builder, res.value, LLVMPointerType(vector_type, 0), "");
		// 		LLVMBuildStore(p->builder, opv, res_ptr);
		// 		return cg_emit_conv(p, cg_emit_load(p, res), type);
		// 	}
		// }

		// if (inline_array_arith) {
		// 	// inline
		// 	for (i32 i = 0; i < count; i++) {
		// 		cgValue e = cg_emit_load(p, cg_emit_array_epi(p, val, i));
		// 		cgValue z = cg_emit_unary_arith(p, op, e, elem_type);
		// 		cg_emit_store(p, cg_emit_array_epi(p, res, i), z);
		// 	}
		// } else {
		// 	auto loop_data = cg_loop_start(p, count, t_i32);

		// 	cgValue e = cg_emit_load(p, cg_emit_array_ep(p, val, loop_data.idx));
		// 	cgValue z = cg_emit_unary_arith(p, op, e, elem_type);
		// 	cg_emit_store(p, cg_emit_array_ep(p, res, loop_data.idx), z);

		// 	cg_loop_end(p, loop_data);
		// }
		// return cg_emit_load(p, res);
	}

	if (op == Token_Xor) {
		GB_ASSERT(x.kind == cgValue_Value);
		cgValue cmp = cg_value(tb_inst_not(p->func, x.node), x.type);
		return cg_emit_conv(p, cmp, type);
	}

	if (op == Token_Not) {
		TB_Node *zero = cg_const_nil(p, x.type).node;
		// PATCH(odin-tilde): comparisons produce TB_TAG_BOOL while bool
		// constants/loads are INTN — widen BOOL to the constant's type
		if (x.node->dt.type == TB_TAG_BOOL && zero->dt.type != TB_TAG_BOOL) {
			x.node = tb_inst_zxt(p->func, x.node, zero->dt);
		}
		// NOTE(odin-tilde): boolean not is x == 0 (was cmp_ne, which made
		// `!x` the identity and broke every `!cond` in the runtime, e.g.
		// safe_add's `!did_overflow` in the temp allocator arena).
		cgValue cmp = cg_value(tb_inst_cmp_eq(p->func, x.node, zero), x.type);
		return cg_emit_conv(p, cmp, type);
	}

	if (op == Token_Sub && is_type_integer(type) && is_type_different_to_arch_endianness(type)) {
		Type *platform_type = integer_endian_type_to_platform_type(type);
		cgValue v = cg_emit_byte_swap(p, x, platform_type);

		cgValue res = cg_value(tb_inst_neg(p->func, v.node), platform_type);
		return cg_emit_byte_swap(p, res, type);
	}

	if (op == Token_Sub && is_type_float(type) && is_type_different_to_arch_endianness(type)) {
		Type *platform_type = integer_endian_type_to_platform_type(type);
		cgValue v = cg_emit_byte_swap(p, x, platform_type);

		cgValue res = cg_value(tb_inst_neg(p->func, v.node), platform_type);
		return cg_emit_byte_swap(p, res, type);
	}

	cgValue res = {};

	if (op == Token_Sub) { // Number negation
		if (is_type_integer(x.type)) {
			res = cg_value(tb_inst_neg(p->func, x.node), x.type);
		} else if (is_type_float(x.type)) {
			if (type_size_of(x.type) == 2) {
				// NOTE(odin-tilde): f16 is an I16 bit pattern; negation is
				// just flipping the sign bit.
				TB_Node *sign = tb_inst_uint(p->func, TB_TYPE_I16, 0x8000);
				res = cg_value(tb_inst_xor(p->func, x.node, sign), x.type);
			} else {
				res = cg_value(tb_inst_neg(p->func, x.node), x.type);
			}
		} else if (is_type_complex(x.type) || is_type_quaternion(x.type)) {
			// NOTE(odin-tilde): negate every element through the address.
			isize n = is_type_complex(x.type) ? 2 : 4;
			cgValue ptr = cg_address_from_load_or_generate_local(p, x);
			cgAddr addr = cg_add_local(p, x.type, nullptr, false);
			cgValue dst = cg_addr_get_ptr(p, addr);
			for (isize i = 0; i < n; i++) {
				cgValue ev = cg_emit_load(p, cg_emit_struct_ep(p, ptr, i));
				ev = cg_emit_unary_arith(p, Token_Sub, ev, ev.type);
				cg_emit_store(p, cg_emit_struct_ep(p, dst, i), ev);
			}
			return cg_addr_load(p, addr);
		} else if (is_type_simd_vector(x.type)) {
			GB_PANIC("TODO(bill): neg simd");
			// Type *elem = base_array_type(x.type);
			// if (is_type_float(elem)) {
			// 	res.value = LLVMBuildFNeg(p->builder, x.value, "");
			// } else {
			// 	res.value = LLVMBuildNeg(p->builder, x.value, "");
			// }
		} else if (is_type_matrix(x.type)) {
			GB_PANIC("TODO(bill): neg matrix");
			// cgValue zero = {};
			// zero.value = LLVMConstNull(cg_type(p->module, type));
			// zero.type = type;
			// return cg_emit_arith_matrix(p, Token_Sub, zero, x, type, true);
		} else {
			GB_PANIC("Unhandled type %s", type_to_string(x.type));
		}
		res.type = x.type;
		return res;
	}

	return res;
}

gb_internal void cg_emit_if(cgProcedure *p, cgValue const &cond, TB_Node *true_region, TB_Node *false_region) {
	GB_ASSERT(cond.kind == cgValue_Value);
	tb_inst_if(p->func, cond.node, true_region, false_region);
}


struct cgLoopData {
	cgAddr  index_addr;
	cgValue index;
	TB_Node *body;
	TB_Node *done;
	TB_Node *loop;
};

gb_internal cgLoopData cg_loop_start(cgProcedure *p, isize count, Type *index_type) {
	cgLoopData data = {};

	cgValue max = cg_const_int(p, index_type, count);

	data.index_addr = cg_add_local(p, index_type, nullptr, true);

	data.body = cg_control_region(p, "loop_body");
	data.done = cg_control_region(p, "loop_done");
	data.loop = cg_control_region_loop(p, "loop_loop");

	cg_emit_goto(p, data.loop);
	tb_inst_set_control(p->func, data.loop);

	data.index = cg_addr_load(p, data.index_addr);

	cgValue cond = cg_emit_comp(p, Token_Lt, data.index, max);
	cg_emit_if(p, cond, data.body, data.done);
	tb_inst_set_control(p->func, data.body);

	return data;
}

gb_internal void cg_loop_end(cgProcedure *p, cgLoopData const &data) {
	if (data.index_addr.addr.node != nullptr) {
		cg_emit_increment(p, data.index_addr.addr);
		cg_emit_goto(p, data.loop);
		tb_inst_set_control(p->func, data.done);
	}
}



gb_internal void cg_build_try_lhs_rhs(cgProcedure *p, Ast *arg, Type *final_type, cgValue *lhs_, cgValue *rhs_) {
	cgValue lhs = {};
	cgValue rhs = {};

	cgValue value = cg_build_expr(p, arg);
	if (value.kind == cgValue_Multi) {
		auto const &values = value.multi->values;
		if (values.count == 2) {
			lhs = values[0];
			rhs = values[1];
		} else {
			rhs = values[values.count-1];
			if (values.count > 1) {
				lhs = cg_value_multi(slice(values, 0, values.count-1), final_type);
			}
		}
	} else if (is_type_tuple(value.type)) {
		// NOTE(odin-tilde): single materialized tuple value (e.g. the (ptr, ok)
		// pair produced by `&m[k]`, see cg_build_unary_and); lb handles this by
		// shape (is_type_tuple) rather than by value kind. Split it: lhs = all
		// but the trailing ok, rhs = the trailing ok.
		isize n = value.type->Tuple.variables.count - 1;
		cgValue addr = cg_address_from_load_or_generate_local(p, value);
		if (n == 1) {
			lhs = cg_emit_load(p, cg_emit_struct_ep(p, addr, 0));
		} else {
			auto vals = slice_make<cgValue>(permanent_allocator(), n);
			for (isize i = 0; i < n; i++) {
				vals[i] = cg_emit_load(p, cg_emit_struct_ep(p, addr, cast(i32)i));
			}
			lhs = cg_value_multi(vals, final_type);
		}
		rhs = cg_emit_load(p, cg_emit_struct_ep(p, addr, cast(i32)n));
	} else {
		rhs = value;
	}

	GB_ASSERT(rhs.node != nullptr);

	if (lhs_) *lhs_ = lhs;
	if (rhs_) *rhs_ = rhs;
}

gb_internal cgValue cg_emit_try_has_value(cgProcedure *p, cgValue rhs) {
	cgValue has_value = {};
	if (is_type_boolean(rhs.type)) {
		has_value = rhs;
	} else {
		GB_ASSERT_MSG(type_has_nil(rhs.type), "%s", type_to_string(rhs.type));
		has_value = cg_emit_comp_against_nil(p, Token_CmpEq, rhs);
	}
	GB_ASSERT(has_value.node != nullptr);
	return has_value;
}

gb_internal cgValue cg_build_or_return(cgProcedure *p, Ast *arg, Type *final_type) {
	cgValue lhs = {};
	cgValue rhs = {};
	cg_build_try_lhs_rhs(p, arg, final_type, &lhs, &rhs);

	TB_Node *return_region   = cg_control_region(p, "or_return_return");
	TB_Node *continue_region = cg_control_region(p, "or_return_continue");

	cgValue cond = cg_emit_try_has_value(p, rhs);
	cg_emit_if(p, cond, continue_region, return_region);
	tb_inst_set_control(p->func, return_region);
	{
		Type *proc_type = base_type(p->type);
		Type *results = proc_type->Proc.results;
		GB_ASSERT(results != nullptr && results->kind == Type_Tuple);
		TypeTuple *tuple = &results->Tuple;

		GB_ASSERT(tuple->variables.count != 0);

		Entity *end_entity = tuple->variables[tuple->variables.count-1];
		rhs = cg_emit_conv(p, rhs, end_entity->type);
		if (p->type->Proc.has_named_results) {
			GB_ASSERT(end_entity->token.string.len != 0);

			// NOTE(bill): store the named values before returning
			cgAddr found = map_must_get(&p->variable_map, end_entity);
			cg_addr_store(p, found, rhs);

			cg_build_return_stmt(p, {});
		} else {
			GB_ASSERT(tuple->variables.count == 1);
			Slice<cgValue> results = {};
			results.data = &rhs;
			results.count = 1;;
			cg_build_return_stmt_internal(p, results);
		}
	}
	tb_inst_set_control(p->func, continue_region);
	if (final_type != nullptr && !is_type_tuple(final_type)) {
		return cg_emit_conv(p, lhs, final_type);
	}
	// PATCH(odin-tilde): tuple case — return the lhs multi (every value but
	// the trailing ok) so that multi-value assignment can expand it;
	// returning an empty value here silently dropped the results.
	return lhs;
}

gb_internal cgValue cg_build_or_else(cgProcedure *p, Ast *arg, Ast *else_expr, Type *final_type) {
	if (arg->state_flags & StateFlag_DirectiveWasFalse) {
		return cg_build_expr(p, else_expr);
	}

	cgValue lhs = {};
	cgValue rhs = {};
	cg_build_try_lhs_rhs(p, arg, final_type, &lhs, &rhs);

	GB_ASSERT(else_expr != nullptr);

	if (is_diverging_expr(else_expr)) {
		TB_Node *then  = cg_control_region(p, "or_else_then");
		TB_Node *else_ = cg_control_region(p, "or_else_else");

		cg_emit_if(p, cg_emit_try_has_value(p, rhs), then, else_);
		// NOTE(bill): else block needs to be straight afterwards to make sure that the actual value is used
		// from the then block
		tb_inst_set_control(p->func, else_);

		cg_build_expr(p, else_expr);

		// PATCH(odin-tilde): the else expression diverges (e.g. `or_else panic(...)`).
		// Terminate its block explicitly or the panic call's control projection is
		// left dangling, which crashes Cuik's CFG construction (create_block finds
		// no control successor for the call proj).
		if (tb_inst_get_control(p->func)) {
			tb_inst_unreachable(p->func);
		}

		tb_inst_set_control(p->func, then);
		return cg_emit_conv(p, lhs, final_type);
	} else {
		TB_Node *then  = cg_control_region(p, "or_else_then");
		TB_Node *done  = cg_control_region(p, "or_else_done"); // NOTE(bill): Append later
		TB_Node *else_ = cg_control_region(p, "or_else_else");

		cg_emit_if(p, cg_emit_try_has_value(p, rhs), then, else_);
		tb_inst_set_control(p->func, then);

		cgValue x = cg_emit_conv(p, lhs, final_type);

		// NOTE(new-tb): explicit phis are gone; join via a stack slot
		size_t slot_size = 0, slot_align = 0;
		tb_get_data_type_size(p->module->mod, x.node->dt, &slot_size, &slot_align);
		TB_Node *slot = tb_inst_local(p->func, cast(TB_CharUnits)slot_size, cast(TB_CharUnits)slot_align);
		tb_inst_store(p->func, x.node->dt, slot, x.node, cast(TB_CharUnits)slot_align, false);

		tb_inst_goto(p->func, done);
		tb_inst_set_control(p->func, else_);

		cgValue y = cg_emit_conv(p, cg_build_expr(p, else_expr), final_type);

		GB_ASSERT(x.kind == y.kind);
		GB_ASSERT(x.node->dt.raw == y.node->dt.raw);
		tb_inst_store(p->func, y.node->dt, slot, y.node, cast(TB_CharUnits)slot_align, false);

		tb_inst_goto(p->func, done);
		tb_inst_set_control(p->func, done);

		cgValue res = {};
		res.kind = x.kind;
		res.type = final_type;
		res.node = tb_inst_load(p->func, x.node->dt, slot, cast(TB_CharUnits)slot_align, false);
		return res;
	}
}


gb_internal isize cg_control_region_pred_count(TB_Node *label) {
	// NOTE(new-tb): labels are TB_SYMBOL_TABLE nodes whose region (input 1)
	// accumulates the predecessor count.
	if (label->type == TB_SYMBOL_TABLE) {
		TB_Node *region = label->inputs[1];
		return region ? region->input_count : 0;
	}
	GB_ASSERT(label->type == TB_REGION);
	GB_ASSERT(label->input_count > 0);
	return label->input_count;
}

gb_internal cgValue cg_build_logical_binary_expr(cgProcedure *p, TokenKind op, Ast *left, Ast *right, Type *final_type) {
	TB_Node *rhs  = cg_control_region(p, "logical_cmp_rhs");
	TB_Node *done = cg_control_region(p, "logical_cmp_done");

	// NOTE(new-tb): explicit phis are gone; the join value lives in a stack
	// slot seeded before the branch tree (the old phi was short_circuit on
	// every short-circuit edge and the rhs value on the fallthrough edge).
	TB_Node *slot = tb_inst_local(p->func, 1, 1);

	cgValue short_circuit = {};
	if (op == Token_CmpAnd) {
		short_circuit = cg_const_bool(p, t_bool, false);
		tb_inst_store(p->func, TB_TYPE_BOOL, slot, short_circuit.node, 1, false);
		cg_build_cond(p, left, rhs, done);
	} else if (op == Token_CmpOr) {
		short_circuit = cg_const_bool(p, t_bool, true);
		tb_inst_store(p->func, TB_TYPE_BOOL, slot, short_circuit.node, 1, false);
		cg_build_cond(p, left, done, rhs);
	}

	if (cg_control_region_pred_count(rhs) == 0) {
		tb_inst_set_control(p->func, done);
		return cg_emit_conv(p, short_circuit, final_type);
	}

	if (cg_control_region_pred_count(done) == 0) {
		tb_inst_set_control(p->func, rhs);
		return cg_build_expr(p, right);
	}

	tb_inst_set_control(p->func, rhs);
	cgValue edge = cg_build_expr(p, right);
	// PATCH(odin-tilde): the rhs may be an INTN bool (load/constant) rather
	// than a TB_TAG_BOOL comparison result — normalize to BOOL for the slot
	if (edge.node->dt.type != TB_TAG_BOOL) {
		TB_Node *zero = tb_inst_uint(p->func, edge.node->dt, 0);
		edge.node = tb_inst_cmp_ne(p->func, edge.node, zero);
	}
	tb_inst_store(p->func, TB_TYPE_BOOL, slot, edge.node, 1, false);

	tb_inst_goto(p->func, done);
	tb_inst_set_control(p->func, done);

	TB_Node *res = tb_inst_load(p->func, TB_TYPE_BOOL, slot, 1, false);
	return cg_emit_conv(p, cg_value(res, t_bool), final_type);
}



gb_internal cgValue cg_emit_matrix_mul(cgProcedure *p, cgValue lhs, cgValue rhs, Type *type) {
	// NOTE(odin-tilde): straightforward triple-loop matmul using logical
	// (row, column) indexing; cg_emit_matrix_epi handles the memory layout.
	Type *xt = base_type(lhs.type);
	Type *yt = base_type(rhs.type);
	Type *rt = base_type(type);

	GB_ASSERT(is_type_matrix(rt));
	GB_ASSERT(is_type_matrix(xt));
	GB_ASSERT(is_type_matrix(yt));
	GB_ASSERT(xt->Matrix.column_count == yt->Matrix.row_count);
	GB_ASSERT(are_types_identical(xt->Matrix.elem, yt->Matrix.elem));

	Type *elem = xt->Matrix.elem;
	i64 R = rt->Matrix.row_count;
	i64 C = rt->Matrix.column_count;
	i64 K = xt->Matrix.column_count;

	cgValue la = cg_address_from_load_or_generate_local(p, lhs);
	cgValue ra = cg_address_from_load_or_generate_local(p, rhs);
	cgAddr res = cg_add_local(p, type, nullptr, false);

	for (i64 i = 0; i < R; i++) {
		for (i64 j = 0; j < C; j++) {
			cgValue acc = {};
			for (i64 k = 0; k < K; k++) {
				cgValue a = cg_emit_load(p, cg_emit_matrix_epi(p, la, i, k));
				cgValue b = cg_emit_load(p, cg_emit_matrix_epi(p, ra, k, j));
				cgValue prod = cg_emit_arith(p, Token_Mul, a, b, elem);
				acc = (k == 0) ? prod : cg_emit_arith(p, Token_Add, acc, prod, elem);
			}
			cg_emit_store(p, cg_emit_matrix_epi(p, res.addr, i, j), acc);
		}
	}
	return cg_addr_load(p, res);
}

gb_internal cgValue cg_emit_matrix_mul_vector(cgProcedure *p, cgValue lhs, cgValue rhs, Type *type) {
	// matrix * vector: result[i] = sum_k m[i,k] * v[k]
	Type *xt = base_type(lhs.type);
	Type *elem = xt->Matrix.elem;
	i64 R = xt->Matrix.row_count;
	i64 C = xt->Matrix.column_count;

	cgValue ma = cg_address_from_load_or_generate_local(p, lhs);
	cgValue va = cg_address_from_load_or_generate_local(p, rhs);
	cgAddr res = cg_add_local(p, type, nullptr, false);

	for (i64 i = 0; i < R; i++) {
		cgValue acc = {};
		for (i64 k = 0; k < C; k++) {
			cgValue a = cg_emit_load(p, cg_emit_matrix_epi(p, ma, i, k));
			cgValue b = cg_emit_load(p, cg_emit_array_epi(p, va, k));
			cgValue prod = cg_emit_arith(p, Token_Mul, a, b, elem);
			acc = (k == 0) ? prod : cg_emit_arith(p, Token_Add, acc, prod, elem);
		}
		cg_emit_store(p, cg_emit_array_epi(p, res.addr, i), acc);
	}
	return cg_addr_load(p, res);
}

gb_internal cgValue cg_emit_vector_mul_matrix(cgProcedure *p, cgValue lhs, cgValue rhs, Type *type) {
	// vector * matrix: result[j] = sum_k v[k] * m[k,j]
	Type *yt = base_type(rhs.type);
	Type *elem = yt->Matrix.elem;
	i64 R = yt->Matrix.row_count;
	i64 C = yt->Matrix.column_count;

	cgValue va = cg_address_from_load_or_generate_local(p, lhs);
	cgValue ma = cg_address_from_load_or_generate_local(p, rhs);
	cgAddr res = cg_add_local(p, type, nullptr, false);

	for (i64 j = 0; j < C; j++) {
		cgValue acc = {};
		for (i64 k = 0; k < R; k++) {
			cgValue a = cg_emit_load(p, cg_emit_array_epi(p, va, k));
			cgValue b = cg_emit_load(p, cg_emit_matrix_epi(p, ma, k, j));
			cgValue prod = cg_emit_arith(p, Token_Mul, a, b, elem);
			acc = (k == 0) ? prod : cg_emit_arith(p, Token_Add, acc, prod, elem);
		}
		cg_emit_store(p, cg_emit_array_epi(p, res.addr, j), acc);
	}
	return cg_addr_load(p, res);
}

gb_internal cgValue cg_emit_arith_matrix(cgProcedure *p, TokenKind op, cgValue lhs, cgValue rhs, Type *type, bool component_wise) {
	// NOTE(odin-tilde): mirrors lb_emit_arith_matrix.
	GB_ASSERT(is_type_matrix(lhs.type) || is_type_matrix(rhs.type));

	if (op == Token_Mul && !component_wise) {
		Type *xt = base_type(lhs.type);
		Type *yt = base_type(rhs.type);

		if (xt->kind == Type_Matrix) {
			if (yt->kind == Type_Matrix) {
				return cg_emit_matrix_mul(p, lhs, rhs, type);
			} else if (is_type_array_like(yt)) {
				return cg_emit_matrix_mul_vector(p, lhs, rhs, type);
			}
		} else if (is_type_array_like(xt)) {
			GB_ASSERT(yt->kind == Type_Matrix);
			return cg_emit_vector_mul_matrix(p, lhs, rhs, type);
		} else {
			// scalar * matrix: broadcast the scalar across all elements
			GB_ASSERT(xt->kind == Type_Basic);
			GB_ASSERT(yt->kind == Type_Matrix);
			GB_ASSERT(is_type_matrix(type));

			Type *elem = yt->Matrix.elem;
			cgValue s = cg_flatten_value(p, cg_emit_conv(p, lhs, elem));
			cgValue ra = cg_address_from_load_or_generate_local(p, rhs);
			cgAddr res = cg_add_local(p, type, nullptr, false);
			i64 n = matrix_type_total_internal_elems(yt);
			for (i64 i = 0; i < n; i++) {
				cgValue v = cg_emit_load(p, cg_emit_array_epi(p, ra, i));
				v = cg_emit_arith(p, Token_Mul, s, v, elem);
				cg_emit_store(p, cg_emit_array_epi(p, res.addr, i), v);
			}
			return cg_addr_load(p, res);
		}
	}

	if (is_type_matrix(lhs.type)) {
		rhs = cg_emit_conv(p, rhs, lhs.type);
	} else {
		lhs = cg_emit_conv(p, lhs, rhs.type);
	}

	Type *xt = base_type(lhs.type);
	Type *yt = base_type(rhs.type);
	GB_ASSERT_MSG(are_types_identical(xt, yt), "%s %.*s %s", type_to_string(lhs.type), LIT(token_strings[op]), type_to_string(rhs.type));
	GB_ASSERT(xt->kind == Type_Matrix);

	Type *elem = xt->Matrix.elem;
	i64 n = matrix_type_total_internal_elems(xt);
	cgValue la = cg_address_from_load_or_generate_local(p, lhs);
	cgValue ra = cg_address_from_load_or_generate_local(p, rhs);

	if (token_is_comparison(op)) {
		GB_ASSERT_MSG(op == Token_CmpEq || op == Token_NotEq, "matrix comparison %.*s", LIT(token_strings[op]));
		cgValue acc = {};
		for (i64 i = 0; i < n; i++) {
			cgValue a = cg_emit_load(p, cg_emit_array_epi(p, la, i));
			cgValue b = cg_emit_load(p, cg_emit_array_epi(p, ra, i));
			cgValue c = cg_emit_comp(p, op, a, b);
			if (i == 0) {
				acc = c;
			} else if (op == Token_CmpEq) {
				acc = cg_emit_arith(p, Token_And, acc, c, t_bool);
			} else {
				acc = cg_emit_arith(p, Token_Or, acc, c, t_bool);
			}
		}
		return cg_emit_conv(p, acc, type);
	}

	cgAddr res = cg_add_local(p, type, nullptr, false);
	for (i64 i = 0; i < n; i++) {
		cgValue a = cg_emit_load(p, cg_emit_array_epi(p, la, i));
		cgValue b = cg_emit_load(p, cg_emit_array_epi(p, ra, i));
		cgValue v = cg_emit_arith(p, op, a, b, elem);
		cg_emit_store(p, cg_emit_array_epi(p, res.addr, i), v);
	}
	return cg_addr_load(p, res);
}



gb_internal cgValue cg_build_binary_expr(cgProcedure *p, Ast *expr) {
	ast_node(be, BinaryExpr, expr);

	TypeAndValue tv = type_and_value_of_expr(expr);

	if (is_type_matrix(be->left->tav.type) || is_type_matrix(be->right->tav.type)) {
		cgValue left = cg_build_expr(p, be->left);
		cgValue right = cg_build_expr(p, be->right);
		return cg_emit_arith_matrix(p, be->op.kind, left, right, default_type(tv.type), false);
	}


	switch (be->op.kind) {
	case Token_Add:
	case Token_Sub:
	case Token_Mul:
	case Token_Quo:
	case Token_Mod:
	case Token_ModMod:
	case Token_And:
	case Token_Or:
	case Token_Xor:
	case Token_AndNot: {
		Type *type = default_type(tv.type);
		cgValue left = cg_build_expr(p, be->left);
		cgValue right = cg_build_expr(p, be->right);
		return cg_emit_arith(p, be->op.kind, left, right, type);
	}

	case Token_Shl:
	case Token_Shr: {
		cgValue left, right;
		Type *type = default_type(tv.type);
		left = cg_build_expr(p, be->left);

		if (cg_is_expr_untyped_const(be->right)) {
			// NOTE(bill): RHS shift operands can still be untyped
			// Just bypass the standard cg_build_expr
			right = cg_expr_untyped_const_to_typed(p, be->right, type);
		} else {
			right = cg_build_expr(p, be->right);
		}
		return cg_emit_arith(p, be->op.kind, left, right, type);
	}

	case Token_CmpEq:
	case Token_NotEq:
		if (is_type_untyped_nil(be->right->tav.type)) {
			// `x == nil` or `x != nil`
			cgValue left = cg_build_expr(p, be->left);
			cgValue cmp = cg_emit_comp_against_nil(p, be->op.kind, left);
			Type *type = default_type(tv.type);
			return cg_emit_conv(p, cmp, type);
		} else if (is_type_untyped_nil(be->left->tav.type)) {
			// `nil == x` or `nil != x`
			cgValue right = cg_build_expr(p, be->right);
			cgValue cmp = cg_emit_comp_against_nil(p, be->op.kind, right);
			Type *type = default_type(tv.type);
			return cg_emit_conv(p, cmp, type);
		}/* else if (cg_is_empty_string_constant(be->right)) {
			// `x == ""` or `x != ""`
			cgValue s = cg_build_expr(p, be->left);
			s = cg_emit_conv(p, s, t_string);
			cgValue len = cg_string_len(p, s);
			cgValue cmp = cg_emit_comp(p, be->op.kind, len, cg_const_int(p->module, t_int, 0));
			Type *type = default_type(tv.type);
			return cg_emit_conv(p, cmp, type);
		} else if (cg_is_empty_string_constant(be->left)) {
			// `"" == x` or `"" != x`
			cgValue s = cg_build_expr(p, be->right);
			s = cg_emit_conv(p, s, t_string);
			cgValue len = cg_string_len(p, s);
			cgValue cmp = cg_emit_comp(p, be->op.kind, len, cg_const_int(p->module, t_int, 0));
			Type *type = default_type(tv.type);
			return cg_emit_conv(p, cmp, type);
		}*/
		/*fallthrough*/
	case Token_Lt:
	case Token_LtEq:
	case Token_Gt:
	case Token_GtEq:
		{
			cgValue left = {};
			cgValue right = {};

			if (be->left->tav.mode == Addressing_Type) {
				left = cg_typeid(p, be->left->tav.type);
			}
			if (be->right->tav.mode == Addressing_Type) {
				right = cg_typeid(p, be->right->tav.type);
			}
			if (left.node == nullptr)  left  = cg_build_expr(p, be->left);
			if (right.node == nullptr) right = cg_build_expr(p, be->right);
			cgValue cmp = cg_emit_comp(p, be->op.kind, left, right);
			Type *type = default_type(tv.type);
			return cg_emit_conv(p, cmp, type);
		}

	case Token_CmpAnd:
	case Token_CmpOr:
		// PATCH(odin-tilde): tv.type may be untyped bool (the checker keeps
		// logical operators untyped like comparisons); normalize to typed
		// bool the same way the comparison branch above does, otherwise
		// cg_emit_conv hits type_size_of(untyped bool) and asserts.
		return cg_build_logical_binary_expr(p, be->op.kind, be->left, be->right, default_type(tv.type));

	case Token_in:
	case Token_not_in:
		{
			cgValue left = cg_build_expr(p, be->left);
			cgValue right = cg_build_expr(p, be->right);
			Type *rt = base_type(right.type);
			if (is_type_pointer(rt)) {
				right = cg_emit_load(p, right);
				rt = base_type(type_deref(rt));
			}

			switch (rt->kind) {
			case Type_Map:
				{
					cgValue map_ptr = cg_address_from_load_or_generate_local(p, right);
					cgValue key = left;
					cgValue ptr = cg_internal_dynamic_map_get_ptr(p, map_ptr, key);
					if (be->op.kind == Token_in) {
						return cg_emit_conv(p, cg_emit_comp_against_nil(p, Token_NotEq, ptr), t_bool);
					} else {
						return cg_emit_conv(p, cg_emit_comp_against_nil(p, Token_CmpEq, ptr), t_bool);
					}
				}
				break;
			case Type_BitSet:
				{
					Type *key_type = rt->BitSet.elem;
					GB_ASSERT(are_types_identical(left.type, key_type));

					Type *it = bit_set_to_int(rt);
					left = cg_emit_conv(p, left, it);
					if (is_type_different_to_arch_endianness(it)) {
						left = cg_emit_byte_swap(p, left, integer_endian_type_to_platform_type(it));
					}

					cgValue lower = cg_const_value(p, left.type, exact_value_i64(rt->BitSet.lower));
					cgValue key = cg_emit_arith(p, Token_Sub, left, lower, left.type);
					cgValue bit = cg_emit_arith(p, Token_Shl, cg_const_int(p, left.type, 1), key, left.type);
					bit = cg_emit_conv(p, bit, it);

					cgValue old_value = cg_emit_transmute(p, right, it);
					cgValue new_value = cg_emit_arith(p, Token_And, old_value, bit, it);

					if (be->op.kind == Token_in) {
						return cg_emit_conv(p, cg_emit_comp(p, Token_NotEq, new_value, cg_const_int(p, new_value.type, 0)), t_bool);
					} else {
						return cg_emit_conv(p, cg_emit_comp(p, Token_CmpEq, new_value, cg_const_int(p, new_value.type, 0)), t_bool);
					}
				}
				break;
			default:
				GB_PANIC("Invalid 'in' type");
			}
			break;
		}
		break;
	default:
		GB_PANIC("Invalid binary expression");
		break;
	}
	return {};
}

gb_internal cgValue cg_build_cond(cgProcedure *p, Ast *cond, TB_Node *true_block, TB_Node *false_block) {
	cond = unparen_expr(cond);

	GB_ASSERT(cond != nullptr);
	GB_ASSERT(true_block  != nullptr);
	GB_ASSERT(false_block != nullptr);

	// Use to signal not to do compile time short circuit for consts
	cgValue no_comptime_short_circuit = {};

	switch (cond->kind) {
	case_ast_node(ue, UnaryExpr, cond);
		if (ue->op.kind == Token_Not) {
			cgValue cond_val = cg_build_cond(p, ue->expr, false_block, true_block);
			return cond_val;
			// if (cond_val.value && LLVMIsConstant(cond_val.value)) {
			// 	return cg_const_bool(p->module, cond_val.type, LLVMConstIntGetZExtValue(cond_val.value) == 0);
			// }
			// return no_comptime_short_circuit;
		}
	case_end;

	case_ast_node(be, BinaryExpr, cond);
		if (be->op.kind == Token_CmpAnd) {
			TB_Node *block = cg_control_region(p, "cmp_and");
			cg_build_cond(p, be->left, block, false_block);
			tb_inst_set_control(p->func, block);
			cg_build_cond(p, be->right, true_block, false_block);
			return no_comptime_short_circuit;
		} else if (be->op.kind == Token_CmpOr) {
			TB_Node *block = cg_control_region(p, "cmp_or");
			cg_build_cond(p, be->left, true_block, block);
			tb_inst_set_control(p->func, block);
			cg_build_cond(p, be->right, true_block, false_block);
			return no_comptime_short_circuit;
		}
	case_end;
	}

	cgValue v = {};
	if (cg_is_expr_untyped_const(cond)) {
		v = cg_expr_untyped_const_to_typed(p, cond, t_bool);
	} else {
		v = cg_build_expr(p, cond);
	}
	cg_emit_if(p, v, true_block, false_block);
	return v;
}

gb_internal cgValue cg_build_expr_internal(cgProcedure *p, Ast *expr);
gb_internal cgValue cg_build_expr(cgProcedure *p, Ast *expr) {
	cg_set_debug_pos_from_node(p, expr);

	u16 prev_state_flags = p->state_flags;
	defer (p->state_flags = prev_state_flags);

	if (expr->state_flags != 0) {
		u16 in = expr->state_flags;
		u16 out = p->state_flags;

		if (in & StateFlag_bounds_check) {
			out |= StateFlag_bounds_check;
			out &= ~StateFlag_no_bounds_check;
		} else if (in & StateFlag_no_bounds_check) {
			out |= StateFlag_no_bounds_check;
			out &= ~StateFlag_bounds_check;
		}

		if (in & StateFlag_type_assert) {
			out |= StateFlag_type_assert;
			out &= ~StateFlag_no_type_assert;
		} else if (in & StateFlag_no_type_assert) {
			out |= StateFlag_no_type_assert;
			out &= ~StateFlag_type_assert;
		}

		p->state_flags = out;
	}


	// IMPORTANT NOTE(bill):
	// Selector Call Expressions (foo->bar(...))
	// must only evaluate `foo` once as it gets transformed into
	// `foo.bar(foo, ...)`
	// And if `foo` is a procedure call or something more complex, storing the value
	// once is a very good idea
	// If a stored value is found, it must be removed from the cache
	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		// cgValue *pp = map_get(&p->selector_values, expr);
		// if (pp != nullptr) {
		// 	cgValue res = *pp;
		// 	map_remove(&p->selector_values, expr);
		// 	return res;
		// }
		// cgAddr *pa = map_get(&p->selector_addr, expr);
		// if (pa != nullptr) {
		// 	cgAddr res = *pa;
		// 	map_remove(&p->selector_addr, expr);
		// 	return cg_addr_load(p, res);
		// }
	}

	cgValue res = cg_build_expr_internal(p, expr);
	if (res.kind == cgValue_Symbol) {
		GB_ASSERT(is_type_internally_pointer_like(res.type));
		res = cg_value(tb_inst_get_symbol_address(p->func, res.symbol), res.type);
	}

	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		// map_set(&p->selector_values, expr, res);
	}
	return res;
}


gb_internal cgValue cg_find_ident(cgProcedure *p, Entity *e, Ast *expr) {
	cgAddr *found_addr = map_get(&p->variable_map, e);
	if (found_addr) {
		return cg_addr_load(p, *found_addr);
	}

	if (cg_entity_is_thread_local(e)) {
		// NOTE(odin-tilde): TLS 变量读也必须走每线程表（写路径在
		// cg_build_addr_from_entity），否则会读到模板全局的零值。
		return cg_addr_load(p, cg_addr(cg_tls_address(p, e)));
	}

	cgValue *found = nullptr;
	rw_mutex_shared_lock(&p->module->values_mutex);
	found = map_get(&p->module->values, e);
	rw_mutex_shared_unlock(&p->module->values_mutex);

	if (found) {

		auto v = *found;
		// NOTE(bill): This is because pointers are already pointers in LLVM
		if (is_type_proc(v.type)) {
			return v;
		}
		return cg_emit_load(p, v);
	} else if (e != nullptr && e->kind == Entity_Variable) {
		return cg_addr_load(p, cg_build_addr(p, expr));
	}

	if (e->kind == Entity_Procedure) {
		return cg_find_procedure_value_from_entity(p->module, e);
	}

	String pkg = {};
	if (e->pkg) {
		pkg = e->pkg->name;
	}
	gb_printf_err("Error in: %s\n", token_pos_to_string(ast_token(expr).pos));
	GB_PANIC("nullptr value for expression from identifier: %.*s.%.*s (%p) : %s @ %p", LIT(pkg), LIT(e->token.string), e, type_to_string(e->type), expr);
	return {};
}

cgAddr cg_build_addr_compound_lit(cgProcedure *p, Ast *expr) {
	struct cgCompoundLitElemTempData {
		Ast *   expr;
		cgValue value;
		i64     elem_index;
		i64     elem_length;
		cgValue gep;
	};


	auto const &populate = [](cgProcedure *p, Slice<Ast *> const &elems, Array<cgCompoundLitElemTempData> *temp_data, Type *compound_type) {
		Type *bt = base_type(compound_type);
		Type *et = nullptr;
		switch (bt->kind) {
		case Type_Array:           et = bt->Array.elem;           break;
		case Type_EnumeratedArray: et = bt->EnumeratedArray.elem; break;
		case Type_Slice:           et = bt->Slice.elem;           break;
		case Type_BitSet:          et = bt->BitSet.elem;          break;
		case Type_DynamicArray:    et = bt->DynamicArray.elem;    break;
		case Type_SimdVector:      et = bt->SimdVector.elem;      break;
		case Type_Matrix:          et = bt->Matrix.elem;          break;
		}
		GB_ASSERT(et != nullptr);


		// NOTE(bill): Separate value, gep, store into their own chunks
		for_array(i, elems) {
			Ast *elem = elems[i];
			if (elem->kind == Ast_FieldValue) {
				ast_node(fv, FieldValue, elem);
				if (is_ast_range(fv->field)) {
					ast_node(ie, BinaryExpr, fv->field);
					TypeAndValue lo_tav = ie->left->tav;
					TypeAndValue hi_tav = ie->right->tav;
					GB_ASSERT(lo_tav.mode == Addressing_Constant);
					GB_ASSERT(hi_tav.mode == Addressing_Constant);

					TokenKind op = ie->op.kind;
					i64 lo = exact_value_to_i64(lo_tav.value);
					i64 hi = exact_value_to_i64(hi_tav.value);
					if (op != Token_RangeHalf) {
						hi += 1;
					}

					cgValue value = cg_emit_conv(p, cg_build_expr(p, fv->value), et);

					GB_ASSERT((hi-lo) > 0);

					if (bt->kind == Type_Matrix) {
						GB_PANIC("TODO(bill): Type_Matrix");
						// for (i64 k = lo; k < hi; k++) {
						// 	cgCompoundLitElemTempData data = {};
						// 	data.value = value;

						// 	data.elem_index = matrix_row_major_index_to_offset(bt, k);
						// 	array_add(temp_data, data);
						// }
					} else {
						enum {MAX_ELEMENT_AMOUNT = 32};
						if ((hi-lo) <= MAX_ELEMENT_AMOUNT) {
							for (i64 k = lo; k < hi; k++) {
								cgCompoundLitElemTempData data = {};
								data.value = value;
								data.elem_index = k;
								array_add(temp_data, data);
							}
						} else {
							cgCompoundLitElemTempData data = {};
							data.value = value;
							data.elem_index = lo;
							data.elem_length = hi-lo;
							array_add(temp_data, data);
						}
					}
				} else {
					auto tav = fv->field->tav;
					GB_ASSERT(tav.mode == Addressing_Constant);
					i64 index = exact_value_to_i64(tav.value);

					cgValue value = cg_emit_conv(p, cg_build_expr(p, fv->value), et);
					GB_ASSERT(!is_type_tuple(value.type));

					cgCompoundLitElemTempData data = {};
					data.value = value;
					data.expr = fv->value;
					if (bt->kind == Type_Matrix) {
						data.elem_index = matrix_row_major_index_to_offset(bt, index);
					} else {
						data.elem_index = index;
					}
					array_add(temp_data, data);
				}

			} else {
				// if (bt->kind != Type_DynamicArray && lb_is_elem_const(elem, et)) {
					// continue;
				// }

				cgValue field_expr = cg_build_expr(p, elem);
				GB_ASSERT(!is_type_tuple(field_expr.type));

				cgValue ev = cg_emit_conv(p, field_expr, et);

				cgCompoundLitElemTempData data = {};
				data.value = ev;
				if (bt->kind == Type_Matrix) {
					data.elem_index = matrix_row_major_index_to_offset(bt, i);
				} else {
					data.elem_index = i;
				}
				array_add(temp_data, data);
			}
		}
	};

	auto const &assign_array = [](cgProcedure *p, Array<cgCompoundLitElemTempData> const &temp_data) {
		for (auto const &td : temp_data) if (td.value.node != nullptr) {
			if (td.elem_length > 0) {
				GB_PANIC("TODO(bill): range");
				// auto loop_data = cg_loop_start(p, cast(isize)td.elem_length, t_i32);
				// {
				// 	cgValue dst = td.gep;
				// 	dst = cg_emit_ptr_offset(p, dst, loop_data.idx);
				// 	cg_emit_store(p, dst, td.value);
				// }
				// cg_loop_end(p, loop_data);
			} else {
				cg_emit_store(p, td.gep, td.value);
			}
		}
	};



	ast_node(cl, CompoundLit, expr);

	Type *type = type_of_expr(expr);
	Type *bt = base_type(type);

	cgAddr v = {};
	if (p->is_startup) {
		v = cg_add_global(p, type, nullptr);
	} else {
		v = cg_add_local(p, type, nullptr, true);
	}

	if (cl->elems.count == 0) {
		// No need to create it
		return v;
	}

	TEMPORARY_ALLOCATOR_GUARD();

	Type *et = nullptr;
	switch (bt->kind) {
	case Type_Array:           et = bt->Array.elem;           break;
	case Type_EnumeratedArray: et = bt->EnumeratedArray.elem; break;
	case Type_Slice:           et = bt->Slice.elem;           break;
	case Type_BitSet:          et = bt->BitSet.elem;          break;
	case Type_SimdVector:      et = bt->SimdVector.elem;      break;
	case Type_Matrix:          et = bt->Matrix.elem;          break;
	}

	String proc_name = {};
	if (p->entity) {
		proc_name = p->entity->token.string;
	}
	TokenPos pos = ast_token(expr).pos;


	switch (bt->kind) {
	default: GB_PANIC("Unknown CompoundLit type: %s", type_to_string(type)); break;

	case Type_BitField: {
		// NOTE(odin-tilde): lower a bit_field compound literal to field-wise
		// stores through the existing cgAddr_BitField machinery (the local is
		// zero-initialized, so unmentioned fields stay zero).
		for (Ast *elem : cl->elems) {
			ast_node(fv, FieldValue, elem);
			InternedString interned = fv->field->Ident.interned;
			Selection sel = lookup_field(bt, interned, false);
			GB_ASSERT(sel.is_bit_field);
			GB_ASSERT(!sel.indirect);
			GB_ASSERT(sel.index.count == 1);
			GB_ASSERT(sel.entity != nullptr);

			i64 index = sel.index[0];
			Entity *f = bt->BitField.fields[index];
			GB_ASSERT(f == sel.entity);
			i64 bit_offset = bt->BitField.bit_offsets[index];
			u8 bit_size = bt->BitField.bit_sizes[index];
			GB_ASSERT(bit_size > 0);

			cgValue field_expr = cg_build_expr(p, fv->value);
			cgValue ev = cg_emit_conv(p, field_expr, f->type);

			cgValue ptr = cg_addr_get_ptr(p, v);
			cgAddr field_addr = cg_addr_bit_field(ptr, f->type, bit_offset, bit_size);
			cg_addr_store(p, field_addr, ev);
		}
	} break;

	case Type_Struct: {
		TypeStruct *st = &bt->Struct;
		cgValue comp_lit_ptr = cg_addr_get_ptr(p, v);

		for_array(field_index, cl->elems) {
			Ast *elem = cl->elems[field_index];

			cgValue field_expr = {};
			Entity *field = nullptr;
			isize index = field_index;

			if (elem->kind == Ast_FieldValue) {
				ast_node(fv, FieldValue, elem);
				InternedString name = fv->field->Ident.interned;
				Selection sel = lookup_field(bt, name, false);
				GB_ASSERT(!sel.indirect);

				elem = fv->value;
				if (sel.index.count > 1) {
					cgValue dst = cg_emit_deep_field_gep(p, comp_lit_ptr, sel);
					field_expr = cg_build_expr(p, elem);
					field_expr = cg_emit_conv(p, field_expr, sel.entity->type);
					cg_emit_store(p, dst, field_expr);
					continue;
				}

				index = sel.index[0];
			} else {
				Selection sel = lookup_field_from_index(bt, st->fields[field_index]->Variable.field_index);
				GB_ASSERT(sel.index.count == 1);
				GB_ASSERT(!sel.indirect);
				index = sel.index[0];
			}

			field = st->fields[index];
			Type *ft = field->type;

			field_expr = cg_build_expr(p, elem);

			cgValue gep = {};
			if (st->is_raw_union) {
				gep = cg_emit_conv(p, comp_lit_ptr, alloc_type_pointer(ft));
			} else {
				gep = cg_emit_struct_ep(p, comp_lit_ptr, cast(i32)index);
			}

			Type *fet = field_expr.type;
			GB_ASSERT(fet->kind != Type_Tuple);

			// HACK TODO(bill): THIS IS A MASSIVE HACK!!!!
			if (is_type_union(ft) && !are_types_identical(fet, ft) && !is_type_untyped(fet)) {
				GB_ASSERT_MSG(union_variant_index_checked(ft, fet) >= 0, "%s", type_to_string(fet));
				GB_ASSERT(are_types_identical(type_deref(gep.type), ft));

				// PATCH(odin-tilde): ported from lb_emit_store_union_variant call site
				cg_emit_store_union_variant(p, gep, field_expr, fet);
			} else {
				cgValue fv = cg_emit_conv(p, field_expr, ft);
				cg_emit_store(p, gep, fv);
			}
		}
		return v;
	}

	case Type_Map: {
		GB_ASSERT(expr->file()->feature_flags & OptInFeatureFlag_DynamicLiterals || build_context.dynamic_literals);

		// NOTE(odin-tilde): mirrors lb_build_addr_compound_lit
		cgValue err = cg_dynamic_map_reserve(p, v.addr, 2*cl->elems.count, expr);
		gb_unused(err);

		for (Ast *elem : cl->elems) {
			ast_node(fv, FieldValue, elem);

			cgValue key   = cg_build_expr(p, fv->field);
			cgValue value = cg_build_expr(p, fv->value);
			cg_internal_dynamic_map_set(p, v.addr, type, key, value, elem);
		}
		break;
	}

	case Type_Array: {
		auto temp_data = array_make<cgCompoundLitElemTempData>(temporary_allocator(), 0, cl->elems.count);

		populate(p, cl->elems, &temp_data, type);

		cgValue dst_ptr = cg_addr_get_ptr(p, v);
		for_array(i, temp_data) {
			i32 index = cast(i32)(temp_data[i].elem_index);
			temp_data[i].gep = cg_emit_array_epi(p, dst_ptr, index);
		}

		assign_array(p, temp_data);
		break;
	}
	case Type_EnumeratedArray: {
		auto temp_data = array_make<cgCompoundLitElemTempData>(temporary_allocator(), 0, cl->elems.count);

		populate(p, cl->elems, &temp_data, type);

		cgValue dst_ptr = cg_addr_get_ptr(p, v);
		i64 index_offset = exact_value_to_i64(*bt->EnumeratedArray.min_value);
		for_array(i, temp_data) {
			i32 index = cast(i32)(temp_data[i].elem_index - index_offset);
			temp_data[i].gep = cg_emit_array_epi(p, dst_ptr, index);
		}

		assign_array(p, temp_data);
		break;
	}
	case Type_Slice: {
		isize count = gb_max(cl->elems.count, cl->max_count);

		TB_CharUnits backing_size = cast(TB_CharUnits)(type_size_of(bt->Slice.elem) * count);
		TB_CharUnits align = cast(TB_CharUnits)type_align_of(bt->Slice.elem);

		TB_Node *backing = nullptr;
		if (p->is_startup) {
			TB_Global *global = nullptr;
			{
				String gen_name = cg_generated_name(p->module, p, "sldata");
				global = tb_global_create(p->module->mod, gen_name.len, cast(char const *)gen_name.text, nullptr, TB_LINKAGE_PRIVATE);
			}
			tb_global_set_storage(p->module->mod, tb_module_get_data(p->module->mod), global, backing_size, align, 0);
			backing = tb_inst_get_symbol_address(p->func, cast(TB_Symbol *)global);
		} else {
			backing = tb_inst_local(p->func, backing_size, align);
			// NOTE(odin-tilde): zero-init the backing store so that holes
			// between range elements read as zero (mirrors cg_add_local)
			TB_Node *zero = tb_inst_uint(p->func, TB_TYPE_I8, 0);
			TB_Node *n    = tb_inst_uint(p->func, TB_TYPE_I32, cast(u64)backing_size);
			tb_inst_memset(p->func, backing, zero, n, align);
		}

		cgValue data = cg_value(backing, alloc_type_multi_pointer(bt->Slice.elem));

		auto temp_data = array_make<cgCompoundLitElemTempData>(temporary_allocator(), 0, cl->elems.count);
		populate(p, cl->elems, &temp_data, type);


		for_array(i, temp_data) {
			temp_data[i].gep = cg_emit_ptr_offset(p, data, cg_const_int(p, t_int, temp_data[i].elem_index));
		}

		assign_array(p, temp_data);
		cg_fill_slice(p, v, data, cg_const_int(p, t_int, count));
		return v;
	}

	case Type_DynamicArray: {
		GB_ASSERT(expr->file()->feature_flags & OptInFeatureFlag_DynamicLiterals || build_context.dynamic_literals);

		Type *et = bt->DynamicArray.elem;
		cgValue size  = cg_const_int(p, t_int, type_size_of(et));
		cgValue align = cg_const_int(p, t_int, type_align_of(et));

		i64 item_count = gb_max(cl->max_count, cl->elems.count);
		{

			auto args = slice_make<cgValue>(temporary_allocator(), 5);
			args[0] = cg_emit_conv(p, cg_addr_get_ptr(p, v), t_rawptr);
			args[1] = size;
			args[2] = align;
			args[3] = cg_const_int(p, t_int, item_count);
			args[4] = cg_emit_source_code_location_as_global(p, proc_name, pos);
			cg_emit_runtime_call(p, "__dynamic_array_reserve", args);
		}

		Type *array_type = alloc_type_array(et, item_count);
		cgAddr items_addr = cg_add_local(p, array_type, nullptr, true);
		cgValue items = cg_addr_get_ptr(p, items_addr);

		auto temp_data = array_make<cgCompoundLitElemTempData>(temporary_allocator(), 0, cl->elems.count);
		populate(p, cl->elems, &temp_data, type);

		for_array(i, temp_data) {
			temp_data[i].gep = cg_emit_array_epi(p, items, temp_data[i].elem_index);
		}
		assign_array(p, temp_data);

		{
			auto args = slice_make<cgValue>(temporary_allocator(), 6);
			args[0] = cg_emit_conv(p, v.addr, t_rawptr);
			args[1] = size;
			args[2] = align;
			args[3] = cg_emit_conv(p, items, t_rawptr);
			args[4] = cg_const_int(p, t_int, item_count);
			args[5] = cg_emit_source_code_location_as_global(p, proc_name, pos);
			cg_emit_runtime_call(p, "__dynamic_array_append", args);
		}
		break;
	}

	case Type_Basic: {
		GB_ASSERT(is_type_any(bt));
		InternedString field_names[2] = {
			string_interner_insert(str_lit("data")),
			string_interner_insert(str_lit("id")),
		};
		Type *field_types[2] = {
			t_rawptr,
			t_typeid,
		};

		for_array(field_index, cl->elems) {
			Ast *elem = cl->elems[field_index];

			cgValue field_expr = {};
			isize index = field_index;

			if (elem->kind == Ast_FieldValue) {
				ast_node(fv, FieldValue, elem);
				Selection sel = lookup_field(bt, fv->field->Ident.interned, false);
				index = sel.index[0];
				elem = fv->value;
			} else {
				TypeAndValue tav = type_and_value_of_expr(elem);
				Selection sel = lookup_field(bt, field_names[field_index], false);
				index = sel.index[0];
			}

			field_expr = cg_build_expr(p, elem);

			GB_ASSERT(field_expr.type->kind != Type_Tuple);

			Type *ft = field_types[index];
			cgValue fv = cg_emit_conv(p, field_expr, ft);
			cgValue gep = cg_emit_struct_ep(p, cg_addr_get_ptr(p, v), index);
			cg_emit_store(p, gep, fv);
		}
		break;
	}

	case Type_BitSet: {
		i64 sz = type_size_of(type);
		if (sz == 0) {
			return v;
		}
		cgValue lower = cg_const_value(p, t_int, exact_value_i64(bt->BitSet.lower));
		Type *it = bit_set_to_int(bt);
		cgValue one = cg_const_value(p, it, exact_value_i64(1));
		for (Ast *elem : cl->elems) {
			GB_ASSERT(elem->kind != Ast_FieldValue);

			cgValue expr = cg_build_expr(p, elem);
			GB_ASSERT(expr.type->kind != Type_Tuple);

			cgValue e = cg_emit_conv(p, expr, it);
			e = cg_emit_arith(p, Token_Sub, e, lower, it);
			e = cg_emit_arith(p, Token_Shl, one, e, it);

			cgValue old_value = cg_emit_transmute(p, cg_addr_load(p, v), it);
			cgValue new_value = cg_emit_arith(p, Token_Or, old_value, e, it);
			new_value = cg_emit_transmute(p, new_value, type);
			cg_addr_store(p, v, new_value);
		}
		return v;
	}

	case Type_Matrix: {
		auto temp_data = array_make<cgCompoundLitElemTempData>(temporary_allocator(), 0, cl->elems.count);

		populate(p, cl->elems, &temp_data, type);

		cgValue dst_ptr = cg_addr_get_ptr(p, v);
		for_array(i, temp_data) {
			temp_data[i].gep = cg_emit_array_epi(p, dst_ptr, temp_data[i].elem_index);
		}

		assign_array(p, temp_data);
		break;
	}

	case Type_SimdVector: {
		// NOTE(odin-tilde): vectors live in memory as flat element arrays in
		// this backend, so a #simd compound literal lowers exactly like the
		// Type_Array case (populate + per-element stores).
		auto temp_data = array_make<cgCompoundLitElemTempData>(temporary_allocator(), 0, cl->elems.count);

		populate(p, cl->elems, &temp_data, type);

		cgValue dst_ptr = cg_addr_get_ptr(p, v);
		for_array(i, temp_data) {
			i32 index = cast(i32)(temp_data[i].elem_index);
			temp_data[i].gep = cg_emit_array_epi(p, dst_ptr, index);
		}

		assign_array(p, temp_data);
		break;
	}
	}

	return v;
}

gb_internal cgValue cg_make_soa_pointer(cgProcedure *p, Type *type, cgValue const &addr, cgValue const &index) {
	cgAddr v = cg_add_local(p, type, nullptr, true);
	cgValue ptr = cg_emit_struct_ep(p, v.addr, 0);
	cgValue idx = cg_emit_struct_ep(p, v.addr, 1);
	cg_emit_store(p, ptr, addr);
	cg_emit_store(p, idx, cg_emit_conv(p, index, t_int));

	return cg_addr_load(p, v);
}

gb_internal cgValue cg_build_unary_and(cgProcedure *p, Ast *expr) {
	ast_node(ue, UnaryExpr, expr);
	auto tv = type_and_value_of_expr(expr);


	Ast *ue_expr = unparen_expr(ue->expr);
	if (ue_expr->kind == Ast_IndexExpr && tv.mode == Addressing_OptionalOkPtr && is_type_tuple(tv.type)) {
		// NOTE(odin-tilde): &m[k] — ported from lb_build_unary_and. Yields the
		// (ptr, ok) tuple: ptr into the map cell (nil if absent), ok = ptr != nil.
		Type *tuple = tv.type;

		Type *map_type = type_of_expr(ue_expr->IndexExpr.expr);
		Type *ot = base_type(map_type);
		Type *t = base_type(type_deref(ot));
		bool deref = t != ot;
		GB_ASSERT(t->kind == Type_Map);
		ast_node(ie, IndexExpr, ue_expr);

		cgValue map_val = cg_build_addr_ptr(p, ie->expr);
		if (deref) {
			map_val = cg_emit_load(p, map_val);
		}

		cgValue key = cg_build_expr(p, ie->index);
		key = cg_emit_conv(p, key, t->Map.key);

		cgAddr addr = cg_addr_map(map_val, key, t, alloc_type_pointer(t->Map.value));
		cgValue ptr = cg_addr_get_ptr(p, addr);

		cgValue ok = cg_emit_comp_against_nil(p, Token_NotEq, ptr);
		ok = cg_emit_conv(p, ok, tuple->Tuple.variables[1]->type);

		cgAddr res = cg_add_local(p, tuple, nullptr, false);
		cgValue gep0 = cg_emit_struct_ep(p, res.addr, 0);
		cgValue gep1 = cg_emit_struct_ep(p, res.addr, 1);
		cg_emit_store(p, gep0, ptr);
		cg_emit_store(p, gep1, ok);
		return cg_addr_load(p, res);

	} else if (is_type_soa_pointer(tv.type)) {
		ast_node(ie, IndexExpr, ue_expr);
		cgValue addr = cg_build_addr_ptr(p, ie->expr);
		cgValue index = cg_build_expr(p, ie->index);

		if (!build_context.no_bounds_check) {
			// TODO(bill): soa bounds checking
		}

		return cg_make_soa_pointer(p, tv.type, addr, index);
	} else if (ue_expr->kind == Ast_CompoundLit) {
		cgAddr addr = cg_build_addr_compound_lit(p, ue_expr);
		return addr.addr;
	} else if (ue_expr->kind == Ast_TypeAssertion) {
		// NOTE(odin-tilde): &v.(T) — mirrors lb_build_unary_and. For
		// any/union, `&x.(T)` on the single-value form IS the data pointer
		// (with a type check); the ,ok form materializes the (ptr, ok) tuple.
		ast_node(ta, TypeAssertion, ue_expr);
		TokenPos pos = ast_token(expr).pos;
		Type *type = type_of_expr(ue_expr);
		GB_ASSERT(!is_type_tuple(type));

		if (is_type_tuple(tv.type)) {
			Type *tuple = tv.type;
			Type *ptr_type = tuple->Tuple.variables[0]->type;
			Type *ok_type = tuple->Tuple.variables[1]->type;

			cgValue e = cg_build_expr(p, ta->expr);
			Type *t = type_deref(e.type);
			if (is_type_union(t)) {
				cgValue v = e;
				if (!is_type_pointer(v.type)) {
					v = cg_address_from_load_or_generate_local(p, v);
				}
				Type *src_type = type_deref(v.type);
				Type *dst_type = type;

				cgValue src_tag = {};
				cgValue dst_tag = {};
				if (is_type_union_maybe_pointer(src_type)) {
					src_tag = cg_emit_comp_against_nil(p, Token_NotEq, v);
					dst_tag = cg_const_bool(p, t_bool, true);
				} else {
					src_tag = cg_emit_load(p, cg_emit_union_tag_ptr(p, v));
					dst_tag = cg_const_union_tag(p, src_type, dst_type);
				}

				cgValue ok = cg_emit_comp(p, Token_CmpEq, src_tag, dst_tag);

				cgValue data_ptr = cg_emit_conv(p, v, ptr_type);
				cgAddr res = cg_add_local(p, tuple, nullptr, true);
				cgValue gep0 = cg_emit_struct_ep(p, res.addr, 0);
				cgValue gep1 = cg_emit_struct_ep(p, res.addr, 1);
				cg_emit_store(p, gep0, cg_emit_select(p, ok, data_ptr, cg_const_nil(p, ptr_type)));
				cg_emit_store(p, gep1, cg_emit_conv(p, ok, ok_type));
				return cg_addr_load(p, res);
			} else if (is_type_any(t)) {
				cgValue v = e;
				if (is_type_pointer(v.type)) {
					v = cg_emit_load(p, v);
				}

				cgValue data_ptr = cg_emit_conv(p, cg_emit_struct_ev(p, v, 0), ptr_type);
				cgValue any_id = cg_emit_struct_ev(p, v, 1);
				cgValue id = cg_typeid(p, type);

				cgValue ok = cg_emit_comp(p, Token_CmpEq, any_id, id);

				cgAddr res = cg_add_local(p, tuple, nullptr, true);
				cgValue gep0 = cg_emit_struct_ep(p, res.addr, 0);
				cgValue gep1 = cg_emit_struct_ep(p, res.addr, 1);
				cg_emit_store(p, gep0, cg_emit_select(p, ok, data_ptr, cg_const_nil(p, ptr_type)));
				cg_emit_store(p, gep1, cg_emit_conv(p, ok, ok_type));
				return cg_addr_load(p, res);
			} else {
				GB_PANIC("TODO(bill): type assertion %s", type_to_string(type));
			}
		} else {
			GB_ASSERT(is_type_pointer(tv.type));

			bool do_type_check = true;
			if (build_context.no_type_assert) {
				// NOTE(odin-tilde): lb also consults per-file feature flags
				// (OptInFeatureFlag_ForceTypeAssert); not wired up here.
				do_type_check = false;
			} else if ((p->state_flags & StateFlag_no_type_assert) != 0) {
				do_type_check = false;
			}

			cgValue e = cg_build_expr(p, ta->expr);
			Type *t = type_deref(e.type);
			if (is_type_union(t)) {
				cgValue v = e;
				if (!is_type_pointer(v.type)) {
					v = cg_address_from_load_or_generate_local(p, v);
				}
				Type *src_type = type_deref(v.type);
				Type *dst_type = type;

				if (do_type_check) {
					cgValue src_tag = {};
					cgValue dst_tag = {};
					if (is_type_union_maybe_pointer(src_type)) {
						src_tag = cg_emit_comp_against_nil(p, Token_NotEq, v);
						dst_tag = cg_const_bool(p, t_bool, true);
					} else {
						src_tag = cg_emit_load(p, cg_emit_union_tag_ptr(p, v));
						dst_tag = cg_const_union_tag(p, src_type, dst_type);
					}

					isize arg_count = 6;
					if (build_context.no_rtti) {
						arg_count = 4;
					}

					cgValue ok = cg_emit_comp(p, Token_CmpEq, src_tag, dst_tag);
					auto args = slice_make<cgValue>(permanent_allocator(), arg_count);
					args[0] = ok;
					args[1] = cg_const_string(p, t_string, get_file_path_string(pos.file_id));
					args[2] = cg_const_int(p, t_i32, pos.line);
					args[3] = cg_const_int(p, t_i32, pos.column);

					if (!build_context.no_rtti) {
						args[4] = cg_typeid(p, src_type);
						args[5] = cg_typeid(p, dst_type);
					}

					char const *name = "type_assertion_check_contextless";
					if (p->context_stack.count > 0) {
						name = "type_assertion_check_with_context";
					}
					cg_emit_runtime_call(p, name, args);
				}

				cgValue data_ptr = v;
				return cg_emit_conv(p, data_ptr, tv.type);
			} else if (is_type_any(t)) {
				cgValue v = e;
				if (is_type_pointer(v.type)) {
					v = cg_emit_load(p, v);
				}
				cgValue data_ptr = cg_emit_struct_ev(p, v, 0);
				if (do_type_check) {
					GB_ASSERT(!build_context.no_rtti);

					cgValue any_id = cg_emit_struct_ev(p, v, 1);

					isize arg_count = 6;
					if (build_context.no_rtti) {
						arg_count = 4;
					}

					cgValue id = cg_typeid(p, type);
					cgValue ok = cg_emit_comp(p, Token_CmpEq, any_id, id);
					auto args = slice_make<cgValue>(permanent_allocator(), arg_count);
					args[0] = ok;
					args[1] = cg_const_string(p, t_string, get_file_path_string(pos.file_id));
					args[2] = cg_const_int(p, t_i32, pos.line);
					args[3] = cg_const_int(p, t_i32, pos.column);

					if (!build_context.no_rtti) {
						args[4] = any_id;
						args[5] = id;
					}

					char const *name = "type_assertion_check_contextless";
					if (p->context_stack.count > 0) {
						name = "type_assertion_check_with_context";
					}
					cg_emit_runtime_call(p, name, args);
				}

				return cg_emit_conv(p, data_ptr, tv.type);
			} else {
				GB_PANIC("TODO(bill): type assertion %s", type_to_string(type));
			}
		}
	}


	return cg_build_addr_ptr(p, ue->expr);
}

gb_internal cgValue cg_emit_cast_union(cgProcedure *p, cgValue value, Type *type, TokenPos pos) {
	Type *src_type = value.type;
	bool is_ptr = is_type_pointer(src_type);

	bool is_tuple = true;
	Type *tuple = type;
	if (type->kind != Type_Tuple) {
		is_tuple = false;
		tuple = make_optional_ok_type(type);
	}


	if (is_ptr) {
		value = cg_emit_load(p, value);
	}
	Type *src = base_type(type_deref(src_type));
	GB_ASSERT_MSG(is_type_union(src), "%s", type_to_string(src_type));
	Type *dst = tuple->Tuple.variables[0]->type;

	cgValue value_  = cg_address_from_load_or_generate_local(p, value);

	if ((p->state_flags & StateFlag_no_type_assert) != 0 && !is_tuple) {
		// just do a bit cast of the data at the front
		cgValue ptr = cg_emit_conv(p, value_, alloc_type_pointer(type));
		return cg_emit_load(p, ptr);
	}


	cgValue tag = {};
	cgValue dst_tag = {};
	cgValue cond = {};
	cgValue data = {};

	cgValue gep0 = cg_add_local(p, tuple->Tuple.variables[0]->type, nullptr, true).addr;
	cgValue gep1 = cg_add_local(p, tuple->Tuple.variables[1]->type, nullptr, true).addr;

	if (is_type_union_maybe_pointer(src)) {
		data = cg_emit_load(p, cg_emit_conv(p, value_, gep0.type));
	} else {
		tag     = cg_emit_load(p, cg_emit_union_tag_ptr(p, value_));
		dst_tag = cg_const_union_tag(p, src, dst);
	}

	TB_Node *ok_block  = cg_control_region(p, "union_cast_ok");
	TB_Node *end_block = cg_control_region(p, "union_cast_end");

	if (data.node != nullptr) {
		GB_ASSERT(is_type_union_maybe_pointer(src));
		cond = cg_emit_comp_against_nil(p, Token_NotEq, data);
	} else {
		cond = cg_emit_comp(p, Token_CmpEq, tag, dst_tag);
	}

	cg_emit_if(p, cond, ok_block, end_block);
	tb_inst_set_control(p->func, ok_block);

	if (data.node == nullptr) {
		data = cg_emit_load(p, cg_emit_conv(p, value_, gep0.type));
	}
	cg_emit_store(p, gep0, data);
	cg_emit_store(p, gep1, cg_const_bool(p, t_bool, true));

	cg_emit_goto(p, end_block);
	tb_inst_set_control(p->func, end_block);

	if (!is_tuple) {
		GB_ASSERT((p->state_flags & StateFlag_no_type_assert) == 0);
		// NOTE(bill): Panic on invalid conversion
		Type *dst_type = tuple->Tuple.variables[0]->type;

		isize arg_count = 7;
		if (build_context.no_rtti) {
			arg_count = 4;
		}

		cgValue ok = cg_emit_load(p, gep1);
		auto args = slice_make<cgValue>(permanent_allocator(), arg_count);
		args[0] = ok;

		args[1] = cg_const_string(p, t_string, get_file_path_string(pos.file_id));
		args[2] = cg_const_int(p, t_i32, pos.line);
		args[3] = cg_const_int(p, t_i32, pos.column);

		if (!build_context.no_rtti) {
			args[4] = cg_typeid(p, src_type);
			args[5] = cg_typeid(p, dst_type);
			args[6] = cg_emit_conv(p, value_, t_rawptr);
		}
		char const *check2_name = "type_assertion_check2_contextless";
		if (p->context_stack.count > 0) {
			check2_name = "type_assertion_check2_with_context";
		}
		cg_emit_runtime_call(p, check2_name, args);

		return cg_emit_load(p, gep0);
	}

	return cg_value_multi2(cg_emit_load(p, gep0), cg_emit_load(p, gep1), tuple);
}

gb_internal cgValue cg_emit_cast_any(cgProcedure *p, cgValue value, Type *type, TokenPos pos) {
	Type *src_type = value.type;

	if (is_type_pointer(src_type)) {
		value = cg_emit_load(p, value);
	}

	bool is_tuple = true;
	Type *tuple = type;
	if (type->kind != Type_Tuple) {
		is_tuple = false;
		tuple = make_optional_ok_type(type);
	}
	Type *dst_type = tuple->Tuple.variables[0]->type;

	if ((p->state_flags & StateFlag_no_type_assert) != 0 && !is_tuple) {
		// just do a bit cast of the data at the front
		cgValue ptr = cg_emit_struct_ev(p, value, 0);
		ptr = cg_emit_conv(p, ptr, alloc_type_pointer(type));
		return cg_emit_load(p, ptr);
	}

	cgValue dst_typeid = cg_typeid(p, dst_type);
	cgValue any_typeid = cg_emit_struct_ev(p, value, 1);


	TB_Node *ok_block = cg_control_region(p, "any_cast_ok");
	TB_Node *end_block = cg_control_region(p, "any_cast_end");
	cgValue cond = cg_emit_comp(p, Token_CmpEq, any_typeid, dst_typeid);
	cg_emit_if(p, cond, ok_block, end_block);
	tb_inst_set_control(p->func, ok_block);

	cgValue gep0 = cg_add_local(p, tuple->Tuple.variables[0]->type, nullptr, true).addr;
	cgValue gep1 = cg_add_local(p, tuple->Tuple.variables[1]->type, nullptr, true).addr;

	cgValue any_data = cg_emit_struct_ev(p, value, 0);
	cgValue ptr = cg_emit_conv(p, any_data, alloc_type_pointer(dst_type));
	cg_emit_store(p, gep0, cg_emit_load(p, ptr));
	cg_emit_store(p, gep1, cg_const_bool(p, t_bool, true));

	cg_emit_goto(p, end_block);
	tb_inst_set_control(p->func, end_block);

	if (!is_tuple) {
		// NOTE(bill): Panic on invalid conversion
		cgValue ok = cg_emit_load(p, gep1);

		isize arg_count = 7;
		if (build_context.no_rtti) {
			arg_count = 4;
		}
		auto args = slice_make<cgValue>(permanent_allocator(), arg_count);
		args[0] = ok;

		args[1] = cg_const_string(p, t_string, get_file_path_string(pos.file_id));
		args[2] = cg_const_int(p, t_i32, pos.line);
		args[3] = cg_const_int(p, t_i32, pos.column);

		if (!build_context.no_rtti) {
			args[4] = any_typeid;
			args[5] = dst_typeid;
			args[6] = cg_emit_struct_ev(p, value, 0);
		}
		char const *check2_name = "type_assertion_check2_contextless";
		if (p->context_stack.count > 0) {
			check2_name = "type_assertion_check2_with_context";
		}
		cg_emit_runtime_call(p, check2_name, args);

		return cg_emit_load(p, gep0);
	}

	return cg_value_multi2(cg_emit_load(p, gep0), cg_emit_load(p, gep1), tuple);
}


gb_internal cgValue cg_build_type_assertion(cgProcedure *p, Ast *expr, Type *type) {
	ast_node(ta, TypeAssertion, expr);

	TokenPos pos = ast_token(expr).pos;
	cgValue e = cg_build_expr(p, ta->expr);
	Type *t = type_deref(e.type);

	if (is_type_union(t)) {
		return cg_emit_cast_union(p, e, type, pos);
	} else if (is_type_any(t)) {
		return cg_emit_cast_any(p, e, type, pos);
	}
	GB_PANIC("TODO(bill): type assertion %s", type_to_string(e.type));
	return {};
}


gb_internal cgValue cg_build_expr_internal(cgProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);

	TokenPos expr_pos = ast_token(expr).pos;
	TypeAndValue tv = type_and_value_of_expr(expr);
	Type *type = type_of_expr(expr);
	GB_ASSERT_MSG(tv.mode != Addressing_Invalid, "invalid expression '%s' (tv.mode = %d, tv.type = %s) @ %s\n Current Proc: %.*s : %s", expr_to_string(expr), tv.mode, type_to_string(tv.type), token_pos_to_string(expr_pos), LIT(p->name), type_to_string(p->type));

	if (tv.value.kind != ExactValue_Invalid &&
	    expr->kind != Ast_CompoundLit) {
		// NOTE(bill): The commented out code below is just for debug purposes only
		// if (is_type_untyped(type)) {
		// 	gb_printf_err("%s %s : %s @ %p\n", token_pos_to_string(expr_pos), expr_to_string(expr), type_to_string(expr->tav.type), expr);
		// 	GB_PANIC("%s\n", type_to_string(tv.type));
		// }
		// NOTE(bill): Short on constant values
		return cg_const_value(p, type, tv.value);
	} else if (tv.mode == Addressing_Type) {
		// NOTE(bill, 2023-01-16): is this correct? I hope so at least
		return cg_typeid(p, tv.type);
	}

	switch (expr->kind) {
	case_ast_node(bl, BasicLit, expr);
		// NOTE(odin-tilde): mirrors lb — os.Error's zero literal does not get a
		// constant tav value (`if err != 0`); emit the nil value for the type.
		if (type != nullptr && type->kind == Type_Named && type->Named.name == "Error") {
			Entity *e = type->Named.type_name;
			if (e->pkg && e->pkg->name == "os") {
				return cg_const_nil(p, type);
			}
		}
		{
			TokenPos pos = bl->token.pos;
			GB_PANIC("Non-constant basic literal %s - %.*s", token_pos_to_string(pos), LIT(token_strings[bl->token.kind]));
		}
	case_end;

	case_ast_node(bd, BasicDirective, expr);
		TokenPos pos = bd->token.pos;
		GB_PANIC("Non-constant basic literal %s - %.*s", token_pos_to_string(pos), LIT(bd->name.string));
	case_end;

	case_ast_node(i, Ident, expr);
		Entity *e = entity_from_expr(expr);
		e = strip_entity_wrapping(e);

		GB_ASSERT_MSG(e != nullptr, "%s in %.*s %p", expr_to_string(expr), LIT(p->name), expr);

		if (e->kind == Entity_Builtin) {
			Token token = ast_token(expr);
			GB_PANIC("TODO(bill): lb_build_expr Entity_Builtin '%.*s'\n"
			         "\t at %s", LIT(builtin_procs[e->Builtin.id].name),
			         token_pos_to_string(token.pos));
			return {};
		} else if (e->kind == Entity_Nil) {
			// TODO(bill): is this correct?
			return cg_value(cast(TB_Node *)nullptr, e->type);
		}
		GB_ASSERT(e->kind != Entity_ProcGroup);

		cgAddr *addr = map_get(&p->variable_map, e);
		if (addr) {
			return cg_addr_load(p, *addr);
		}
		return cg_find_ident(p, e, expr);
	case_end;

	case_ast_node(i, Implicit, expr);
		return cg_addr_load(p, cg_build_addr(p, expr));
	case_end;

	case_ast_node(u, Uninit, expr);
		if (is_type_untyped(type)) {
			return cg_value(cast(TB_Node *)nullptr, t_untyped_uninit);
		}
		return cg_value(tb_inst_poison(p->func, cg_data_type(type)), type);
	case_end;

	case_ast_node(de, DerefExpr, expr);
		return cg_addr_load(p, cg_build_addr(p, expr));
	case_end;


	case_ast_node(se, SelectorExpr, expr);
		TypeAndValue tav = type_and_value_of_expr(expr);
		GB_ASSERT(tav.mode != Addressing_Invalid);
		return cg_addr_load(p, cg_build_addr(p, expr));
	case_end;

	case_ast_node(ise, ImplicitSelectorExpr, expr);
		TypeAndValue tav = type_and_value_of_expr(expr);
		GB_ASSERT(tav.mode == Addressing_Constant);

		return cg_const_value(p, type, tv.value);
	case_end;


	case_ast_node(se, SelectorCallExpr, expr);
		GB_ASSERT(se->modified_call);
		return cg_build_call_expr(p, se->call);
	case_end;

	case_ast_node(i, CallExpr, expr);
		return cg_build_call_expr(p, expr);
	case_end;

	case_ast_node(cl, CompoundLit, expr);
		cgAddr addr = cg_build_addr_compound_lit(p, expr);
		return cg_addr_load(p, addr);
	case_end;


	case_ast_node(te, TernaryIfExpr, expr);
		TB_Node *then  = cg_control_region(p, "if_then");
		TB_Node *done  = cg_control_region(p, "if_done");
		TB_Node *else_ = cg_control_region(p, "if_else");

		cg_build_cond(p, te->cond, then, else_);
		tb_inst_set_control(p->func, then);

		Type *type = default_type(type_of_expr(expr));

		cgValue x = cg_emit_conv(p, cg_build_expr(p, te->x), type);

		// NOTE(new-tb): explicit phis are gone; join via a stack slot
		TB_DataType dt0 = (x.kind == cgValue_Addr) ? TB_TYPE_PTR : x.node->dt;
		size_t slot_size = 0, slot_align = 0;
		tb_get_data_type_size(p->module->mod, dt0, &slot_size, &slot_align);
		TB_Node *slot = tb_inst_local(p->func, cast(TB_CharUnits)slot_size, cast(TB_CharUnits)slot_align);
		tb_inst_store(p->func, dt0, slot, x.node, cast(TB_CharUnits)slot_align, false);

		cg_emit_goto(p, done);
		tb_inst_set_control(p->func, else_);

		cgValue y = cg_emit_conv(p, cg_build_expr(p, te->y), type);
		tb_inst_store(p->func, dt0, slot, y.node, cast(TB_CharUnits)slot_align, false);

		cg_emit_goto(p, done);
		tb_inst_set_control(p->func, done);

		GB_ASSERT(x.kind == cgValue_Value ||
		          x.kind == cgValue_Addr);
		GB_ASSERT(x.kind == y.kind);

		cgValue res = {};
		res.kind = x.kind;
		res.type = type;
		res.node = tb_inst_load(p->func, dt0, slot, cast(TB_CharUnits)slot_align, false);
		return res;
	case_end;

	case_ast_node(te, TernaryWhenExpr, expr);
		TypeAndValue tav = type_and_value_of_expr(te->cond);
		GB_ASSERT(tav.mode == Addressing_Constant);
		GB_ASSERT(tav.value.kind == ExactValue_Bool);
		if (tav.value.value_bool) {
			return cg_build_expr(p, te->x);
		} else {
			return cg_build_expr(p, te->y);
		}
	case_end;

	case_ast_node(tc, TypeCast, expr);
		cgValue e = cg_build_expr(p, tc->expr);
		switch (tc->token.kind) {
		case Token_cast:
			return cg_emit_conv(p, e, type);
		case Token_transmute:
			return cg_emit_transmute(p, e, type);
		}
		GB_PANIC("Invalid AST TypeCast");
	case_end;

	case_ast_node(ac, AutoCast, expr);
		cgValue value = cg_build_expr(p, ac->expr);
		return cg_emit_conv(p, value, type);
	case_end;

	case_ast_node(se, SliceExpr, expr);
		if (is_type_slice(type_of_expr(se->expr))) {
			// NOTE(bill): Quick optimization
			if (se->high == nullptr &&
			    (se->low == nullptr || cg_is_expr_constant_zero(se->low))) {
				return cg_build_expr(p, se->expr);
			}
		}
		return cg_addr_load(p, cg_build_addr(p, expr));
	case_end;

	case_ast_node(ie, IndexExpr, expr);
		return cg_addr_load(p, cg_build_addr(p, expr));
	case_end;

	case_ast_node(ie, MatrixIndexExpr, expr);
		return cg_addr_load(p, cg_build_addr(p, expr));
	case_end;

	case_ast_node(ue, UnaryExpr, expr);
		if (ue->op.kind == Token_And) {
			return cg_build_unary_and(p, expr);
		}
		cgValue v = cg_build_expr(p, ue->expr);
		return cg_emit_unary_arith(p, ue->op.kind, v, type);
	case_end;
	case_ast_node(be, BinaryExpr, expr);
		return cg_build_binary_expr(p, expr);
	case_end;

	case_ast_node(oe, OrReturnExpr, expr);
		return cg_build_or_return(p, oe->expr, tv.type);
	case_end;

	case_ast_node(oe, OrElseExpr, expr);
		return cg_build_or_else(p, oe->x, oe->y, tv.type);
	case_end;

	case_ast_node(be, OrBranchExpr, expr);
		TB_Node *block = nullptr;

		if (be->label != nullptr) {
			cgBranchRegions bb = cg_lookup_branch_regions(p, be->label);
			switch (be->token.kind) {
			case Token_or_break:    block = bb.break_;    break;
			case Token_or_continue: block = bb.continue_; break;
			}
		} else {
			for (cgTargetList *t = p->target_list; t != nullptr && block == nullptr; t = t->prev) {
				if (t->is_block) {
					continue;
				}

				switch (be->token.kind) {
				case Token_or_break:    block = t->break_;    break;
				case Token_or_continue: block = t->continue_; break;
				}
			}
		}

		GB_ASSERT(block != nullptr);

		cgValue lhs = {};
		cgValue rhs = {};
		cg_build_try_lhs_rhs(p, be->expr, tv.type, &lhs, &rhs);
		Type *def_type = default_type(tv.type);
		if (lhs.node != nullptr) {
			lhs = cg_emit_conv(p, lhs, def_type);
		} else if (def_type != nullptr && def_type != t_invalid) {
			lhs = cg_const_nil(p, def_type);
		}

		TB_Node *then  = cg_control_region(p, "or_branch.then");
		TB_Node *else_ = cg_control_region(p, "or_branch.else");

		cg_emit_if(p, cg_emit_try_has_value(p, rhs), then, else_);
		tb_inst_set_control(p->func, else_);
		cg_emit_defer_stmts(p, cgDeferExit_Branch, block);
		cg_emit_goto(p, block);
		tb_inst_set_control(p->func, then);

		return lhs;
	case_end;

	case_ast_node(ta, TypeAssertion, expr);
		return cg_build_type_assertion(p, expr, tv.type);
	case_end;

	case_ast_node(pl, ProcLit, expr);
		cgProcedure *anon = cg_procedure_generate_anonymous(p->module, expr, p);
		GB_ASSERT(anon != nullptr);
		GB_ASSERT(anon->symbol != nullptr);
		return cg_value(tb_inst_get_symbol_address(p->func, anon->symbol), type);
	case_end;

	}
	TokenPos token_pos = ast_token(expr).pos;
	GB_PANIC("Unexpected expression\n"
	         "\tAst: %.*s @ "
	         "%s\n",
	         LIT(ast_strings[expr->kind]),
	         token_pos_to_string(token_pos));

	return {};
}


gb_internal cgValue cg_map_data_uintptr(cgProcedure *p, cgValue value) {
	GB_ASSERT(is_type_map(value.type) || are_types_identical(value.type, t_raw_map));
	cgValue data = cg_emit_struct_ev(p, value, 0);
	u64 mask_value = 0;
	if (build_context.ptr_size == 4) {
		mask_value = 0xfffffffful & ~(MAP_CACHE_LINE_SIZE-1);
	} else {
		mask_value = 0xffffffffffffffffull & ~(MAP_CACHE_LINE_SIZE-1);
	}
	cgValue mask = cg_const_int(p, t_uintptr, mask_value);
	return cg_emit_arith(p, Token_And, data, mask, t_uintptr);
}

gb_internal cgValue cg_gen_map_key_hash(cgProcedure *p, cgValue const &map_ptr, cgValue key, cgValue *key_ptr_) {
	TEMPORARY_ALLOCATOR_GUARD();

	cgValue key_ptr = cg_address_from_load_or_generate_local(p, key);
	key_ptr = cg_emit_conv(p, key_ptr, t_rawptr);

	if (key_ptr_) *key_ptr_ = key_ptr;

	Type* key_type = base_type(type_deref(map_ptr.type))->Map.key;

	cgValue hasher = cg_hasher_proc_value_for_type(p, key_type);

	Slice<cgValue> args = {};
	args = slice_make<cgValue>(temporary_allocator(), 1);
	args[0] = cg_map_data_uintptr(p, cg_emit_load(p, map_ptr));
	cgValue seed = cg_emit_runtime_call(p, "map_seed_from_map_data", args);

	args = slice_make<cgValue>(temporary_allocator(), 2);
	args[0] = key_ptr;
	args[1] = seed;
	return cg_emit_call(p, hasher, args);
}

gb_internal cgValue cg_internal_dynamic_map_get_ptr(cgProcedure *p, cgValue const &map_ptr, cgValue const &key) {
	TEMPORARY_ALLOCATOR_GUARD();

	Type *map_type = base_type(type_deref(map_ptr.type));
	GB_ASSERT(map_type->kind == Type_Map);

	cgValue ptr = {};
	cgValue key_ptr = {};
	cgValue hash = cg_gen_map_key_hash(p, map_ptr, key, &key_ptr);

	auto args = slice_make<cgValue>(temporary_allocator(), 4);
	args[0] = cg_emit_transmute(p, map_ptr, t_raw_map_ptr);
	args[1] = cg_builtin_map_info(p, map_type);
	args[2] = hash;
	args[3] = key_ptr;

	ptr = cg_emit_runtime_call(p, "__dynamic_map_get", args);

	return cg_emit_conv(p, ptr, alloc_type_pointer(map_type->Map.value));
}


gb_internal cgValue cg_dynamic_map_reserve(cgProcedure *p, cgValue const &map_ptr, isize const capacity, Ast *node) {
	TEMPORARY_ALLOCATOR_GUARD();

	auto args = slice_make<cgValue>(temporary_allocator(), 4);
	args[0] = cg_emit_conv(p, map_ptr, t_rawptr);
	args[1] = cg_builtin_map_info(p, base_type(type_deref(map_ptr.type)));
	args[2] = cg_const_int(p, t_uint, capacity);
	args[3] = cg_emit_source_code_location_as_global(p, node);
	return cg_emit_runtime_call(p, "__dynamic_map_reserve", args);
}

gb_internal void cg_internal_dynamic_map_set(cgProcedure *p, cgValue const &map_ptr, Type *map_type,
                                             cgValue const &map_key, cgValue const &map_value, Ast *node) {
	TEMPORARY_ALLOCATOR_GUARD();

	map_type = base_type(map_type);
	GB_ASSERT(map_type->kind == Type_Map);

	cgValue key_ptr = {};
	cgValue hash = cg_gen_map_key_hash(p, map_ptr, map_key, &key_ptr);

	cgValue v = cg_emit_conv(p, map_value, map_type->Map.value);
	cgValue value_ptr = cg_address_from_load_or_generate_local(p, v);

	auto args = slice_make<cgValue>(temporary_allocator(), 6);
	args[0] = cg_emit_conv(p, map_ptr, t_raw_map_ptr);
	args[1] = cg_builtin_map_info(p, map_type);
	args[2] = hash;
	args[3] = cg_emit_conv(p, key_ptr, t_rawptr);
	args[4] = cg_emit_conv(p, value_ptr, t_rawptr);
	args[5] = cg_emit_source_code_location_as_global(p, node);
	cg_emit_runtime_call(p, "__dynamic_map_set", args);
}




gb_internal cgValue cg_build_addr_ptr(cgProcedure *p, Ast *expr) {
	cgAddr addr = cg_build_addr(p, expr);
	return cg_addr_get_ptr(p, addr);
}

gb_internal cgAddr cg_build_addr_internal(cgProcedure *p, Ast *expr);
gb_internal cgAddr cg_build_addr(cgProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);

	// IMPORTANT NOTE(bill):
	// Selector Call Expressions (foo->bar(...))
	// must only evaluate `foo` once as it gets transformed into
	// `foo.bar(foo, ...)`
	// And if `foo` is a procedure call or something more complex, storing the value
	// once is a very good idea
	// If a stored value is found, it must be removed from the cache
	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		// lbAddr *pp = map_get(&p->selector_addr, expr);
		// if (pp != nullptr) {
		// 	lbAddr res = *pp;
		// 	map_remove(&p->selector_addr, expr);
		// 	return res;
		// }
	}
	cgAddr addr = cg_build_addr_internal(p, expr);
	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		// map_set(&p->selector_addr, expr, addr);
	}
	return addr;
}

gb_internal cgAddr cg_build_addr_index_expr(cgProcedure *p, Ast *expr) {
	ast_node(ie, IndexExpr, expr);

	Type *t = base_type(type_of_expr(ie->expr));

	bool deref = is_type_pointer(t);
	t = base_type(type_deref(t));
	if (is_type_soa_struct(t)) {
		cgValue val = cg_build_addr_ptr(p, ie->expr);
		if (deref) {
			val = cg_emit_load(p, val);
		}

		cgValue index = cg_build_expr(p, ie->index);
		return cg_addr_soa_variable(val, index, ie->index);
	}

	if (ie->expr->tav.mode == Addressing_SoaVariable) {
		GB_PANIC("TODO(bill): #soa");
		// // SOA Structures for slices/dynamic arrays
		// GB_ASSERT(is_type_pointer(type_of_expr(ie->expr)));

		// lbValue field = lb_build_expr(p, ie->expr);
		// lbValue index = lb_build_expr(p, ie->index);


		// if (!build_context.no_bounds_check) {
		// 	// TODO HACK(bill): Clean up this hack to get the length for bounds checking
		// 	// GB_ASSERT(LLVMIsALoadInst(field.value));

		// 	// lbValue a = {};
		// 	// a.value = LLVMGetOperand(field.value, 0);
		// 	// a.type = alloc_type_pointer(field.type);

		// 	// irInstr *b = &a->Instr;
		// 	// GB_ASSERT(b->kind == irInstr_StructElementPtr);
		// 	// lbValue base_struct = b->StructElementPtr.address;

		// 	// GB_ASSERT(is_type_soa_struct(type_deref(ir_type(base_struct))));
		// 	// lbValue len = ir_soa_struct_len(p, base_struct);
		// 	// lb_emit_bounds_check(p, ast_token(ie->index), index, len);
		// }
		// lbValue val = lb_emit_ptr_offset(p, field, index);
		// return lb_addr(val);
	}

	GB_ASSERT_MSG(is_type_indexable(t), "%s %s", type_to_string(t), expr_to_string(expr));

	if (is_type_map(t)) {
		cgAddr map_addr = cg_build_addr(p, ie->expr);
		cgValue key = cg_build_expr(p, ie->index);
		key = cg_emit_conv(p, key, t->Map.key);

		Type *result_type = type_of_expr(expr);
		cgValue map_ptr = cg_addr_get_ptr(p, map_addr);
		if (is_type_pointer(type_deref(map_ptr.type))) {
			map_ptr = cg_emit_load(p, map_ptr);
		}
		return cg_addr_map(map_ptr, key, t, result_type);
	}

	switch (t->kind) {
	case Type_Array: {
		cgValue array = {};
		array = cg_build_addr_ptr(p, ie->expr);
		if (deref) {
			array = cg_emit_load(p, array);
		}
		cgValue index = cg_build_expr(p, ie->index);
		index = cg_emit_conv(p, index, t_int);
		cgValue elem = cg_emit_array_ep(p, array, index);

		auto index_tv = type_and_value_of_expr(ie->index);
		if (index_tv.mode != Addressing_Constant) {
			// cgValue len = cg_const_int(p->module, t_int, t->Array.count);
			// cg_emit_bounds_check(p, ast_token(ie->index), index, len);
		}
		return cg_addr(elem);
	}

	case Type_EnumeratedArray: {
		cgValue array = {};
		array = cg_build_addr_ptr(p, ie->expr);
		if (deref) {
			array = cg_emit_load(p, array);
		}

		Type *index_type = t->EnumeratedArray.index;

		auto index_tv = type_and_value_of_expr(ie->index);

		cgValue index = {};
		if (compare_exact_values(Token_NotEq, *t->EnumeratedArray.min_value, exact_value_i64(0))) {
			if (index_tv.mode == Addressing_Constant) {
				ExactValue idx = exact_value_sub(index_tv.value, *t->EnumeratedArray.min_value);
				index = cg_const_value(p, index_type, idx);
			} else {
				index = cg_emit_arith(p, Token_Sub,
				                      cg_build_expr(p, ie->index),
				                      cg_const_value(p, index_type, *t->EnumeratedArray.min_value),
				                      index_type);
				index = cg_emit_conv(p, index, t_int);
			}
		} else {
			index = cg_emit_conv(p, cg_build_expr(p, ie->index), t_int);
		}

		cgValue elem = cg_emit_array_ep(p, array, index);

		if (index_tv.mode != Addressing_Constant) {
			// cgValue len = cg_const_int(p->module, t_int, t->EnumeratedArray.count);
			// cg_emit_bounds_check(p, ast_token(ie->index), index, len);
		}
		return cg_addr(elem);
	}

	case Type_Slice: {
		cgValue slice = {};
		slice = cg_build_expr(p, ie->expr);
		if (deref) {
			slice = cg_emit_load(p, slice);
		}
		cgValue elem = cg_builtin_raw_data(p, slice);
		cgValue index = cg_emit_conv(p, cg_build_expr(p, ie->index), t_int);
		// cgValue len = cg_builtin_len(p, slice);
		// cg_emit_bounds_check(p, ast_token(ie->index), index, len);
		cgValue v = cg_emit_ptr_offset(p, elem, index);
		v.type = alloc_type_pointer(type_deref(v.type, true));
		return cg_addr(v);
	}

	case Type_MultiPointer: {
		cgValue multi_ptr = {};
		multi_ptr = cg_build_expr(p, ie->expr);
		if (deref) {
			multi_ptr = cg_emit_load(p, multi_ptr);
		}
		cgValue index = cg_build_expr(p, ie->index);
		index = cg_emit_conv(p, index, t_int);

		cgValue v = cg_emit_ptr_offset(p, multi_ptr, index);
		v.type = alloc_type_pointer(type_deref(v.type, true));
		return cg_addr(v);
	}

	case Type_DynamicArray: {
		cgValue dynamic_array = {};
		dynamic_array = cg_build_expr(p, ie->expr);
		if (deref) {
			dynamic_array = cg_emit_load(p, dynamic_array);
		}
		cgValue elem = cg_builtin_raw_data(p, dynamic_array);
		cgValue index = cg_emit_conv(p, cg_build_expr(p, ie->index), t_int);
		// cgValue len = cg_dynamic_array_len(p, dynamic_array);
		// cg_emit_bounds_check(p, ast_token(ie->index), index, len);
		cgValue v = cg_emit_ptr_offset(p, elem, index);
		v.type = alloc_type_pointer(type_deref(v.type, true));
		return cg_addr(v);
	}

	case Type_FixedCapacityDynamicArray: {
		// NOTE(odin-tilde): the data buffer is inline at offset 0, so
		// &a[i] == base + i*elem_size (mirror the Type_Array path's stride).
		cgAddr base_addr = cg_build_addr(p, ie->expr);
		cgValue base_ptr = cg_addr_get_ptr(p, base_addr);
		cgValue index = cg_emit_conv(p, cg_build_expr(p, ie->index), t_int);
		i64 stride = type_size_of(t->FixedCapacityDynamicArray.elem);
		TB_Node *node = tb_inst_array_access(p->func, base_ptr.node, index.node, stride);
		return cg_addr(cg_value(node, alloc_type_pointer(t->FixedCapacityDynamicArray.elem)));
	}

	case Type_Matrix: {
		GB_PANIC("TODO(bill): matrix");
		// lbValue matrix = {};
		// matrix = lb_build_addr_ptr(p, ie->expr);
		// if (deref) {
		// 	matrix = lb_emit_load(p, matrix);
		// }
		// lbValue index = lb_build_expr(p, ie->index);
		// index = lb_emit_conv(p, index, t_int);
		// lbValue elem = lb_emit_matrix_ep(p, matrix, lb_const_int(p->module, t_int, 0), index);
		// elem = lb_emit_conv(p, elem, alloc_type_pointer(type_of_expr(expr)));

		// auto index_tv = type_and_value_of_expr(ie->index);
		// if (index_tv.mode != Addressing_Constant) {
		// 	lbValue len = lb_const_int(p->module, t_int, t->Matrix.column_count);
		// 	lb_emit_bounds_check(p, ast_token(ie->index), index, len);
		// }
		// return lb_addr(elem);
	}


	case Type_Basic: { // Basic_string
		cgValue str;
		cgValue elem;
		cgValue len;
		cgValue index;

		str = cg_build_expr(p, ie->expr);
		if (deref) {
			str = cg_emit_load(p, str);
		}
		elem = cg_builtin_raw_data(p, str);
		len = cg_builtin_len(p, str);

		index = cg_emit_conv(p, cg_build_expr(p, ie->index), t_int);
		// cg_emit_bounds_check(p, ast_token(ie->index), index, len);

		cgValue v = cg_emit_ptr_offset(p, elem, index);
		v.type = alloc_type_pointer(type_deref(v.type, true));
		return cg_addr(v);
	}
	}
	return {};
}

gb_internal cgAddr cg_build_addr_internal(cgProcedure *p, Ast *expr) {
	switch (expr->kind) {
	case_ast_node(i, Implicit, expr);
		cgAddr v = {};
		switch (i->kind) {
		case Token_context:
			v = cg_find_or_generate_context_ptr(p);
			break;
		}

		GB_ASSERT(v.addr.node != nullptr);
		return v;
	case_end;

	case_ast_node(i, Ident, expr);
		if (is_blank_ident(expr)) {
			cgAddr val = {};
			return val;
		}
		String name = i->token.string;
		Entity *e = entity_of_node(expr);
		return cg_build_addr_from_entity(p, e, expr);
	case_end;

	case_ast_node(de, DerefExpr, expr);
		Type *t = type_of_expr(de->expr);
		if (is_type_soa_pointer(t)) {
			cgValue value = cg_build_expr(p, de->expr);
			cgValue ptr = cg_emit_struct_ev(p, value, 0);
			cgValue idx = cg_emit_struct_ev(p, value, 1);
			return cg_addr_soa_variable(ptr, idx, nullptr);
		}
		cgValue addr = cg_build_expr(p, de->expr);
		return cg_addr(addr);
	case_end;

	case_ast_node(ie, IndexExpr, expr);
		return cg_build_addr_index_expr(p, expr);
	case_end;

	case_ast_node(se, SliceExpr, expr);
		return cg_build_addr_slice_expr(p, expr);
	case_end;

	case_ast_node(se, SelectorExpr, expr);
		Ast *sel_node = unparen_expr(se->selector);
		if (sel_node->kind != Ast_Ident) {
			GB_PANIC("Unsupported selector expression");
		}
		String selector = sel_node->Ident.token.string;
		TypeAndValue tav = type_and_value_of_expr(se->expr);

		if (tav.mode == Addressing_Invalid) {
			// NOTE(bill): Imports
			Entity *imp = entity_of_node(se->expr);
			if (imp != nullptr) {
				GB_ASSERT(imp->kind == Entity_ImportName);
			}
			return cg_build_addr(p, unparen_expr(se->selector));
		}


		Type *type = base_type(tav.type);
		if (tav.mode == Addressing_Type) { // Addressing_Type
			Selection sel = lookup_field(tav.type, sel_node->Ident.interned, true);
			if (sel.pseudo_field) {
				GB_ASSERT(sel.entity->kind == Entity_Procedure);
				return cg_addr(cg_find_value_from_entity(p->module, sel.entity));
			}
			GB_PANIC("Unreachable %.*s", LIT(selector));
		}

		if (se->swizzle_count > 0) {
			Type *array_type = base_type(type_deref(tav.type));
			GB_ASSERT(array_type->kind == Type_Array);
			u8 swizzle_count = se->swizzle_count;
			u8 swizzle_indices_raw = se->swizzle_indices;
			u8 swizzle_indices[4] = {};
			for (u8 i = 0; i < swizzle_count; i++) {
				u8 index = swizzle_indices_raw>>(i*2) & 3;
				swizzle_indices[i] = index;
			}
			cgValue a = {};
			if (is_type_pointer(tav.type)) {
				a = cg_build_expr(p, se->expr);
			} else {
				cgAddr addr = cg_build_addr(p, se->expr);
				a = cg_addr_get_ptr(p, addr);
			}

			Type *swizzle_result_type = type_deref(expr->tav.type);
			GB_ASSERT(is_type_array(swizzle_result_type) || is_type_simd_vector(swizzle_result_type));
			return cg_addr_swizzle(a, swizzle_result_type, swizzle_count, swizzle_indices);
		}

		Selection sel = lookup_field(type, sel_node->Ident.interned, false);
		GB_ASSERT(sel.entity != nullptr);
		if (sel.pseudo_field) {
			GB_ASSERT(sel.entity->kind == Entity_Procedure);
			Entity *e = entity_of_node(sel_node);
			return cg_addr(cg_find_value_from_entity(p->module, e));
		}

		{
			cgAddr addr = cg_build_addr(p, se->expr);
			if (sel.is_bit_field) {
				// NOTE(odin-tilde): mirrors lb — walk the sub-selection to
				// the bit_field value, then hand back a BitField addr.
				Selection sub_sel = sel;
				sub_sel.index.count -= 1;

				cgValue ptr = cg_addr_get_ptr(p, addr);
				if (sub_sel.index.count > 0) {
					ptr = cg_emit_deep_field_gep(p, ptr, sub_sel);
				}
				if (is_type_pointer(type_deref(ptr.type))) {
					ptr = cg_emit_load(p, ptr);
				}

				Type *bf_type = base_type(type_deref(ptr.type));
				GB_ASSERT(bf_type->kind == Type_BitField);

				i32 index = cast(i32)sel.index[sel.index.count-1];
				Entity *f = bf_type->BitField.fields[index];
				u8 bit_size = bf_type->BitField.bit_sizes[index];
				i64 bit_offset = bf_type->BitField.bit_offsets[index];

				return cg_addr_bit_field(ptr, f->type, bit_offset, bit_size);
			}
			if (addr.kind == cgAddr_Map) {
				cgValue v = cg_addr_load(p, addr);
				cgValue a = cg_address_from_load_or_generate_local(p, v);
				a = cg_emit_deep_field_gep(p, a, sel);
				return cg_addr(a);
			} else if (addr.kind == cgAddr_Context) {
				GB_ASSERT(sel.index.count > 0);
				if (addr.ctx.sel.index.count >= 0) {
					sel = selection_combine(addr.ctx.sel, sel);
				}
				addr.ctx.sel = sel;
				addr.kind = cgAddr_Context;
				return addr;
			} else if (addr.kind == cgAddr_SoaVariable) {
				cgValue index = addr.soa.index;
				i64 first_index = sel.index[0];
				Selection sub_sel = sel;
				sub_sel.index.data += 1;
				sub_sel.index.count -= 1;

				cgValue arr = cg_emit_struct_ep(p, addr.addr, first_index);

				Type *t = base_type(type_deref(addr.addr.type));
				GB_ASSERT(is_type_soa_struct(t));

				// TODO(bill): bounds checking for soa variable
				// if (addr.soa.index_expr != nullptr && (!cg_is_const(addr.soa.index) || t->Struct.soa_kind != StructSoa_Fixed)) {
				// 	cgValue len = cg_soa_struct_len(p, addr.addr);
				// 	cg_emit_bounds_check(p, ast_token(addr.soa.index_expr), addr.soa.index, len);
				// }

				cgValue item = {};

				if (t->Struct.soa_kind == StructSoa_Fixed) {
					item = cg_emit_array_ep(p, arr, index);
				} else {
					item = cg_emit_ptr_offset(p, cg_emit_load(p, arr), index);
				}
				if (sub_sel.index.count > 0) {
					item = cg_emit_deep_field_gep(p, item, sub_sel);
				}
				item.type = alloc_type_pointer(type_deref(item.type, true));
				return cg_addr(item);
			} else if (addr.kind == cgAddr_Swizzle) {
				GB_ASSERT(sel.index.count > 0);
				// NOTE(bill): just patch the index in place
				sel.index[0] = addr.swizzle.indices[sel.index[0]];
			} else if (addr.kind == cgAddr_SwizzleLarge) {
				GB_ASSERT(sel.index.count > 0);
				// NOTE(bill): just patch the index in place
				sel.index[0] = addr.swizzle.indices[sel.index[0]];
			}

			cgValue a = cg_addr_get_ptr(p, addr);
			a = cg_emit_deep_field_gep(p, a, sel);
			return cg_addr(a);
		}
	case_end;

	case_ast_node(ce, CallExpr, expr);
		cgValue res = cg_build_expr(p, expr);
		switch (res.kind) {
		case cgValue_Value:
			return cg_addr(cg_address_from_load_or_generate_local(p, res));
		case cgValue_Addr:
			return cg_addr(res);
		case cgValue_Multi:
			GB_PANIC("cannot address a multi-valued expression");
			break;
		}
	case_end;

	case_ast_node(ta, TypeAssertion, expr);
		return cg_addr(cg_address_from_load_or_generate_local(p, cg_build_expr(p, expr)));
	case_end;

	case_ast_node(tc, TypeCast, expr);
		// NOTE(odin-tilde): mirrors lb_build_addr_internal — an rvalue cast
		// (e.g. `(transmute([2]u64)x)[0]`) is addressed through a generated local
		{
			Type *type = type_of_expr(expr);
			cgValue x = cg_build_expr(p, tc->expr);
			cgValue e = {};
			switch (tc->token.kind) {
			case Token_cast:
				e = cg_emit_conv(p, x, type);
				break;
			case Token_transmute:
				e = cg_emit_transmute(p, x, type);
				break;
			default:
				GB_PANIC("Invalid AST TypeCast");
			}
			return cg_addr(cg_address_from_load_or_generate_local(p, e));
		}
	case_end;

	case_ast_node(ac, AutoCast, expr);
		return cg_build_addr(p, ac->expr);
	case_end;

	case_ast_node(cl, CompoundLit, expr);
		return cg_build_addr_compound_lit(p, expr);
	case_end;

	case_ast_node(ue, UnaryExpr, expr);
		// NOTE(odin-tilde): mirrors lb_build_addr_internal — address of `&x`
		// (e.g. iterating `&m` by reference) is the pointer value itself,
		// materialized to an lvalue.
		switch (ue->op.kind) {
		case Token_And:
			return cg_addr(cg_address_from_load_or_generate_local(p, cg_build_expr(p, expr)));
		default:
			GB_PANIC("Invalid unary expression for cg_build_addr");
		}
	case_end;

	case_ast_node(ie, MatrixIndexExpr, expr);
		// NOTE(odin-tilde): mirrors lb_build_addr_internal's
		// MatrixIndexExpr case (minus the bounds check, consistent with
		// the rest of this backend).
		{
			Type *t = base_type(type_of_expr(ie->expr));
			bool deref = is_type_pointer(t);
			cgValue m = cg_build_addr_ptr(p, ie->expr);
			if (deref) {
				m = cg_emit_load(p, m);
			}
			cgValue row_index    = cg_emit_conv(p, cg_build_expr(p, ie->row_index), t_int);
			cgValue column_index = cg_emit_conv(p, cg_build_expr(p, ie->column_index), t_int);
			cgValue elem = cg_emit_matrix_ep(p, m, row_index, column_index);
			return cg_addr(elem);
		}
	case_end;

	}

	TokenPos token_pos = ast_token(expr).pos;
	GB_PANIC("Unexpected address expression\n"
	         "\tAst: %.*s @ "
	         "%s\n",
	         LIT(ast_strings[expr->kind]),
	         token_pos_to_string(token_pos));

	return {};
}