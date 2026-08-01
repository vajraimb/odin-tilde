// NOTE(odin-tilde): TB only has RELAXED/ACQ_REL/SEQ_CST; on x64 acquire,
// release and acq_rel all lower identically, so they all map to ACQ_REL.
// Values mirror OdinAtomicMemoryOrder in types.cpp:
//   relaxed=0, consume=1, acquire=2, release=3, acq_rel=4, seq_cst=5
gb_internal TB_MemoryOrder cg_tb_memory_order_from_odin(Ast *expr) {
	ExactValue value = type_and_value_of_expr(expr).value;
	GB_ASSERT(value.kind == ExactValue_Integer);
	switch (exact_value_to_i64(value)) {
	case 0: return TB_MEM_ORDER_RELAXED;
	case 1:
	case 2:
	case 3:
	case 4: return TB_MEM_ORDER_ACQ_REL;
	case 5: return TB_MEM_ORDER_SEQ_CST;
	}
	return TB_MEM_ORDER_SEQ_CST;
}

gb_internal cgValue cg_builtin_len(cgProcedure *p, cgValue value) {
	Type *t = base_type(value.type);

	switch (t->kind) {
	case Type_Basic:
		switch (t->Basic.kind) {
		case Basic_string:
		case Basic_string16:
			{
				GB_ASSERT(value.kind == cgValue_Addr);
				cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
				cgValue len_ptr = cg_emit_struct_ep(p, ptr, 1);
				return cg_emit_load(p, len_ptr);
			}
		case Basic_cstring:
			{
				// NOTE(odin-tilde): mirrors lb_cstring_len — runtime call.
				auto args = slice_make<cgValue>(permanent_allocator(), 1);
				args[0] = value;
				return cg_emit_runtime_call(p, "cstring_len", args);
			}
		}
		break;
	case Type_Array:
		return cg_const_int(p, t_int, t->Array.count);
	case Type_EnumeratedArray:
		return cg_const_int(p, t_int, t->EnumeratedArray.count);
	case Type_Slice:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue len_ptr = cg_emit_struct_ep(p, ptr, 1);
			return cg_emit_load(p, len_ptr);
		}
	case Type_DynamicArray:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue len_ptr = cg_emit_struct_ep(p, ptr, 1);
			return cg_emit_load(p, len_ptr);
		}
	case Type_FixedCapacityDynamicArray:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue len_ptr = cg_emit_struct_ep(p, ptr, 1);
			return cg_emit_load(p, len_ptr);
		}
	case Type_Map:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue len_ptr = cg_emit_struct_ep(p, ptr, 1);
			return cg_emit_conv(p, cg_emit_load(p, len_ptr), t_int);
		}
	case Type_Struct:
		GB_ASSERT(is_type_soa_struct(t));
		{
			if (t->Struct.soa_kind == StructSoa_Fixed) {
				return cg_const_int(p, t_int, t->Struct.soa_count);
			}

			GB_ASSERT(t->Struct.soa_kind == StructSoa_Slice ||
			          t->Struct.soa_kind == StructSoa_Dynamic);

			isize n = 0;
			Type *elem = base_type(t->Struct.soa_elem);
			if (elem->kind == Type_Struct) {
				n = cast(isize)elem->Struct.fields.count;
			} else if (elem->kind == Type_Array) {
				n = cast(isize)elem->Array.count;
			} else {
				GB_PANIC("Unreachable");
			}

			return cg_emit_struct_ev(p, value, n);
		}
		break;
	}

	GB_PANIC("TODO(bill): cg_builtin_len %s", type_to_string(t));
	return {};
}

gb_internal cgValue cg_builtin_cap(cgProcedure *p, cgValue value) {
	Type *t = base_type(value.type);

	switch (t->kind) {
	case Type_Basic:
		switch (t->Basic.kind) {
		case Basic_string:
			{
				GB_ASSERT(value.kind == cgValue_Addr);
				cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
				cgValue len_ptr = cg_emit_struct_ep(p, ptr, 1);
				return cg_emit_load(p, len_ptr);
			}
		case Basic_cstring:
			GB_PANIC("TODO(bill): cap(cstring)");
			break;
		}
		break;
	case Type_Array:
		return cg_const_int(p, t_int, t->Array.count);
	case Type_EnumeratedArray:
		return cg_const_int(p, t_int, t->EnumeratedArray.count);
	case Type_Slice:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue len_ptr = cg_emit_struct_ep(p, ptr, 1);
			return cg_emit_load(p, len_ptr);
		}
	case Type_DynamicArray:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue len_ptr = cg_emit_struct_ep(p, ptr, 2);
			return cg_emit_load(p, len_ptr);
		}
	case Type_FixedCapacityDynamicArray:
		return cg_const_int(p, t_int, t->FixedCapacityDynamicArray.capacity);
	case Type_Map:
		{
			TB_DataType dt_uintptr = cg_data_type(t_uintptr);
			TB_Node *zero = tb_inst_uint(p->func, dt_uintptr, 0);
			TB_Node *one  = tb_inst_uint(p->func, dt_uintptr, 0);
			TB_Node *mask = tb_inst_uint(p->func, dt_uintptr, MAP_CACHE_LINE_SIZE-1);

			TB_Node *data = cg_emit_struct_ev(p, value, 0).node;
			TB_Node *log2_cap = tb_inst_and(p->func, data, mask);
			TB_Node *cap = tb_inst_shl(p->func, one, log2_cap, cast(TB_ArithmeticBehavior)0);
			TB_Node *cmp = tb_inst_cmp_eq(p->func, data, zero);

			cgValue res = cg_value(tb_inst_select(p->func, cmp, zero, cap), t_uintptr);
			return cg_emit_conv(p, res, t_int);
		}
	case Type_Struct:
		GB_ASSERT(is_type_soa_struct(t));
		{
			// NOTE(odin-tilde): mirrors lb_soa_struct_cap — fixed #soa cap
			// is constant; dynamic #soa layout is fields..., len, cap,
			// allocator, so cap sits at elem_field_count+1.
			if (t->Struct.soa_kind == StructSoa_Fixed) {
				return cg_const_int(p, t_int, t->Struct.soa_count);
			}

			GB_ASSERT(t->Struct.soa_kind == StructSoa_Dynamic);

			isize n = 0;
			Type *elem = base_type(t->Struct.soa_elem);
			if (elem->kind == Type_Struct) {
				n = cast(isize)elem->Struct.fields.count;
			} else if (elem->kind == Type_Array) {
				n = cast(isize)elem->Array.count;
			} else {
				GB_PANIC("Unreachable");
			}

			return cg_emit_struct_ev(p, value, cast(i32)(n+1));
		}
		break;
	}

	GB_PANIC("TODO(bill): cg_builtin_cap %s", type_to_string(t));
	return {};
}


gb_internal cgValue cg_builtin_raw_data(cgProcedure *p, cgValue const &value) {
	Type *t = base_type(value.type);
	cgValue res = {};
	switch (t->kind) {
	case Type_Slice:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue data_ptr = cg_emit_struct_ep(p, ptr, 0);
			res = cg_emit_load(p, data_ptr);
			GB_ASSERT(is_type_multi_pointer(res.type));
		}
		break;
	case Type_DynamicArray:
		{
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue data_ptr = cg_emit_struct_ep(p, ptr, 0);
			res = cg_emit_load(p, data_ptr);
		}
		break;
	case Type_Basic:
		if (t->Basic.kind == Basic_string) {
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue data_ptr = cg_emit_struct_ep(p, ptr, 0);
			res = cg_emit_load(p, data_ptr);
		} else if (t->Basic.kind == Basic_string16) {
			GB_ASSERT(value.kind == cgValue_Addr);
			cgValue ptr = cg_value(value.node, alloc_type_pointer(value.type));
			cgValue data_ptr = cg_emit_struct_ep(p, ptr, 0);
			res = cg_emit_load(p, data_ptr);
		} else if (t->Basic.kind == Basic_cstring) {
			res = cg_emit_conv(p, value, t_u8_multi_ptr);
		} else if (t->Basic.kind == Basic_cstring16) {
			res = cg_emit_conv(p, value, t_u16_multi_ptr);
		}
		break;
	case Type_Pointer:
		if (is_type_fixed_capacity_dynamic_array(t->Pointer.elem)) {
			// NOTE(odin-tilde): FCDA data buffer is inline at offset 0, so a
			// ^[dynamic; N]T pointer IS the element data pointer already.
			TB_Node *base = nullptr;
			if (value.kind == cgValue_Symbol) {
				base = tb_inst_get_symbol_address(p->func, value.symbol);
			} else {
				GB_ASSERT(value.kind == cgValue_Value || value.kind == cgValue_Addr);
				base = value.node;
			}
			res = cg_value(base, alloc_type_multi_pointer(t->Pointer.elem->FixedCapacityDynamicArray.elem));
			break;
		}
		GB_ASSERT(is_type_array_like(t->Pointer.elem));
		{
			// NOTE(odin-tilde): arrays live in memory, so a ^[N]T value may
			// arrive as cgValue_Addr (node IS the pointer value) or as
			// cgValue_Symbol (a global, materialize its address here).
			TB_Node *base = nullptr;
			if (value.kind == cgValue_Symbol) {
				base = tb_inst_get_symbol_address(p->func, value.symbol);
			} else {
				GB_ASSERT(value.kind == cgValue_Value || value.kind == cgValue_Addr);
				base = value.node;
			}
			res = cg_value(base, alloc_type_multi_pointer(base_array_type(t->Pointer.elem)));
		}
		break;
	case Type_MultiPointer:

		GB_PANIC("TODO(bill) %s", type_to_string(value.type));
		// res = cg_emit_conv(p, value, tv.type);
		break;
	}
	GB_ASSERT(res.node != nullptr);
	return res;
}

gb_internal cgValue cg_builtin_min(cgProcedure *p, Type *t, cgValue x, cgValue y) {
	x = cg_emit_conv(p, x, t);
	y = cg_emit_conv(p, y, t);
	return cg_emit_select(p, cg_emit_comp(p, Token_Lt, x, y), x, y);
}
gb_internal cgValue cg_builtin_max(cgProcedure *p, Type *t, cgValue x, cgValue y) {
	x = cg_emit_conv(p, x, t);
	y = cg_emit_conv(p, y, t);
	return cg_emit_select(p, cg_emit_comp(p, Token_Gt, x, y), x, y);
}

gb_internal cgValue cg_builtin_abs(cgProcedure *p, cgValue const &x) {
	if (is_type_unsigned(x.type)) {
		return x;
	}

	if (cg_is_type_integer_128(x.type)) {
		// NOTE(odin-tilde): branchless |x| across the limbs: select per limb
		cgValue neg = cg_emit_unary_arith(p, Token_Sub, x, x.type);
		cgValue cond = cg_emit_comp_u128(p, Token_Lt, x, cg_const_nil(p, x.type));
		bool swapped = cg_u128_is_swapped(x.type);
		TB_Node *xp = cg_u128_addr_of(p, x);
		TB_Node *np = cg_u128_addr_of(p, neg);
		TB_Node *r0 = tb_inst_select(p->func, cond.node,
		                             cg_emit_u128_limb_load(p, np, swapped, 0),
		                             cg_emit_u128_limb_load(p, xp, swapped, 0));
		TB_Node *r1 = tb_inst_select(p->func, cond.node,
		                             cg_emit_u128_limb_load(p, np, swapped, 1),
		                             cg_emit_u128_limb_load(p, xp, swapped, 1));
		return cg_u128_make_value(p, x.type, swapped, r0, r1);
	}

	if (is_type_quaternion(x.type)) {
		// NOTE(odin-tilde): runtime procs compute the magnitude
		i64 sz = 8*type_size_of(x.type);
		auto args = slice_make<cgValue>(permanent_allocator(), 1);
		args[0] = x;
		switch (sz) {
		case 64:  return cg_emit_runtime_call(p, "abs_quaternion64", args);
		case 128: return cg_emit_runtime_call(p, "abs_quaternion128", args);
		case 256: return cg_emit_runtime_call(p, "abs_quaternion256", args);
		}
		GB_PANIC("Unknown quaternion type");
	} else if (is_type_complex(x.type)) {
		i64 sz = 8*type_size_of(x.type);
		auto args = slice_make<cgValue>(permanent_allocator(), 1);
		args[0] = x;
		switch (sz) {
		case 32:  return cg_emit_runtime_call(p, "abs_complex32", args);
		case 64:  return cg_emit_runtime_call(p, "abs_complex64", args);
		case 128: return cg_emit_runtime_call(p, "abs_complex128", args);
		}
		GB_PANIC("Unknown complex type");
	}

	TB_DataType dt = cg_data_type(x.type);
	GB_ASSERT(!TB_IS_VOID_TYPE(dt));
	TB_Node *zero = nullptr;
	if (dt.type == TB_TAG_F32) {
		zero = tb_inst_float32(p->func, 0);
	} else if (dt.type == TB_TAG_F64) {
		zero = tb_inst_float64(p->func, 0);
	} else {
		zero = tb_inst_uint(p->func, dt, 0);
	}
	GB_ASSERT(zero != nullptr);

	cgValue cond = cg_emit_comp(p, Token_Lt, x, cg_value(zero, x.type));
	cgValue neg = cg_emit_unary_arith(p, Token_Sub, x, x.type);
	return cg_emit_select(p, cond, neg, x);
}

gb_internal cgValue cg_builtin_clamp(cgProcedure *p, Type *t, cgValue const &x, cgValue const &min, cgValue const &max) {
	cgValue z = x;
	z = cg_builtin_max(p, t, z, min);
	z = cg_builtin_min(p, t, z, max);
	return z;
}



gb_internal cgValue cg_builtin_mem_zero(cgProcedure *p, cgValue const &ptr, cgValue const &len) {
	GB_ASSERT(ptr.kind == cgValue_Value);
	GB_ASSERT(len.kind == cgValue_Value);
	tb_inst_memzero(p->func, ptr.node, len.node, 1);
	return ptr;
}

gb_internal cgValue cg_builtin_mem_copy(cgProcedure *p, cgValue const &dst, cgValue const &src, cgValue const &len) {
	GB_ASSERT(dst.kind == cgValue_Value);
	GB_ASSERT(src.kind == cgValue_Value);
	GB_ASSERT(len.kind == cgValue_Value);
	// TODO(bill): This needs to be memmove
	tb_inst_memcpy(p->func, dst.node, src.node, len.node, 1);
	return dst;
}

gb_internal cgValue cg_builtin_mem_copy_non_overlapping(cgProcedure *p, cgValue const &dst, cgValue const &src, cgValue const &len) {
	GB_ASSERT(dst.kind == cgValue_Value);
	GB_ASSERT(src.kind == cgValue_Value);
	GB_ASSERT(len.kind == cgValue_Value);
	tb_inst_memcpy(p->func, dst.node, src.node, len.node, 1);
	return dst;
}

gb_internal TB_Symbol *cg_builtin_map_cell_info_symbol(cgModule *m, Type *type) {
	MUTEX_GUARD(&m->map_info_mutex);
	TB_Symbol **found = map_get(&m->map_cell_info_map, type);
	if (found) {
		return *found;
	}
	i64 size = 0, len = 0;
	map_cell_size_and_len(type, &size, &len);

	String mci_name = cg_typed_generated_name("mapci", type);
	// NOTE(odin-tilde): dedup structurally identical twins by name (see
	// cgModule.generated_globals_by_name).
	StringHashKey mci_key = string_hash_string(mci_name);
	TB_Symbol **by_name = string_map_get(&m->generated_globals_by_name, mci_key);
	if (by_name) {
		map_set(&m->map_cell_info_map, type, *by_name);
		return *by_name;
	}
	TB_Global *global = tb_global_create(m->mod, mci_name.len, cast(char const *)mci_name.text, cg_debug_type(m, t_map_cell_info), TB_LINKAGE_PRIVATE);
	string_map_set(&m->generated_globals_by_name, mci_name, cast(TB_Symbol *)global);
	tb_global_set_storage(m->mod, tb_module_get_rdata(m->mod), global, type_size_of(t_map_cell_info), type_align_of(t_map_cell_info), 4);

	i64 ptr_size = build_context.ptr_size;
	void *size_of_type      = tb_global_add_region(m->mod, global, 0*ptr_size, ptr_size);
	void *align_of_type     = tb_global_add_region(m->mod, global, 1*ptr_size, ptr_size);
	void *size_of_cell      = tb_global_add_region(m->mod, global, 2*ptr_size, ptr_size);
	void *elements_per_cell = tb_global_add_region(m->mod, global, 3*ptr_size, ptr_size);

	cg_write_uint_at_ptr(size_of_type,      type_size_of(type),  t_uintptr);
	cg_write_uint_at_ptr(align_of_type,     type_align_of(type), t_uintptr);
	cg_write_uint_at_ptr(size_of_cell,      size,                t_uintptr);
	cg_write_uint_at_ptr(elements_per_cell, len,                 t_uintptr);

	map_set(&m->map_cell_info_map, type, cast(TB_Symbol *)global);

	return cast(TB_Symbol *)global;
}


gb_internal cgValue cg_builtin_map_cell_info(cgProcedure *p, Type *type) {
	type = core_type(type);
	TB_Symbol *symbol = cg_builtin_map_cell_info_symbol(p->module, type);
	TB_Node *node = tb_inst_get_symbol_address(p->func, symbol);
	return cg_value(node, t_map_cell_info_ptr);
}

gb_internal cgValue cg_builtin_map_info(cgProcedure *p, Type *map_type) {
	map_type = base_type(map_type);
	GB_ASSERT(map_type->kind == Type_Map);

	cgModule *m = p->module;
	MUTEX_GUARD(&m->map_info_mutex);
	TB_Global *global = nullptr;
	TB_Symbol **found = map_get(&m->map_info_map, map_type);
	if (found) {
		global = cast(TB_Global *)*found;
	} else {
		String mi_name = cg_typed_generated_name("mapi", map_type);
		StringHashKey mi_key = string_hash_string(mi_name);
		TB_Symbol **by_name = string_map_get(&m->generated_globals_by_name, mi_key);
		if (by_name) {
			// NOTE(odin-tilde): structurally identical twin (see
			// cgModule.generated_globals_by_name).
			global = cast(TB_Global *)*by_name;
			map_set(&m->map_info_map, map_type, *by_name);
		} else {
		global = tb_global_create(m->mod, mi_name.len, cast(char const *)mi_name.text, cg_debug_type(m, t_map_info), TB_LINKAGE_PRIVATE);
		string_map_set(&m->generated_globals_by_name, mi_name, cast(TB_Symbol *)global);
		tb_global_set_storage(m->mod, tb_module_get_rdata(m->mod), global, type_size_of(t_map_info), type_align_of(t_map_info), 4);

		TB_Symbol *key_cell_info   = cg_builtin_map_cell_info_symbol(m, map_type->Map.key);
		TB_Symbol *value_cell_info = cg_builtin_map_cell_info_symbol(m, map_type->Map.value);
		cgProcedure *key_hasher    = cg_hasher_proc_for_type(p->module, map_type->Map.key);
		cgProcedure *key_equal     = cg_equal_proc_for_type (p->module, map_type->Map.key);

		tb_global_add_symbol_reloc(p->module->mod, global, 0*build_context.ptr_size, key_cell_info);
		tb_global_add_symbol_reloc(p->module->mod, global, 1*build_context.ptr_size, value_cell_info);
		tb_global_add_symbol_reloc(p->module->mod, global, 2*build_context.ptr_size, key_hasher->symbol);
		tb_global_add_symbol_reloc(p->module->mod, global, 3*build_context.ptr_size, key_equal->symbol);

		map_set(&m->map_info_map, map_type, cast(TB_Symbol *)global);
		}
	}

	GB_ASSERT(global != nullptr);
	TB_Node *node = tb_inst_get_symbol_address(p->func, cast(TB_Symbol *)global);
	return cg_value(node, t_map_info_ptr);
}

// PATCH(odin-tilde): scalarized #simd intrinsics.
// TB has no general vector ops, so #simd values live in memory (as fixed
// arrays) and every simd intrinsic is expanded into a per-lane scalar loop
// at codegen time. Semantics mirror the LLVM backend (llvm_backend_proc.cpp):
//   - lanes_* produce all-ones/all-zero integer masks (sext of the compare)
//   - select treats any non-zero cond lane as true
gb_internal cgValue cg_simd_base_ptr(cgProcedure *p, cgValue v, Type **out_elem, i64 *out_count) {
	Type *vt = base_type(v.type);
	GB_ASSERT_MSG(vt->kind == Type_SimdVector, "%s", type_to_string(v.type));
	Type *et = vt->SimdVector.elem;
	if (out_elem)  *out_elem  = et;
	if (out_count) *out_count = vt->SimdVector.count;
	cgValue addr = cg_address_from_load_or_generate_local(p, v); // ^vector
	return cg_value(addr.node, alloc_type_pointer(et));
}

gb_internal TB_Node *cg_simd_load_lane(cgProcedure *p, cgValue base_ptr, Type *et, TB_Node *index) {
	cgValue ep = cg_emit_ptr_offset(p, base_ptr, cg_value(index, t_int));
	TB_DataType edt = cg_data_type(et);
	TB_CharUnits align = cast(TB_CharUnits)type_align_of(et);
	return tb_inst_load(p->func, edt, ep.node, align, false);
}

gb_internal cgValue cg_build_builtin_simd_proc(cgProcedure *p, Ast *expr, BuiltinProcId id) {
	ast_node(ce, CallExpr, expr);
	Type *result_type = type_and_value_of_expr(expr).type;

	switch (id) {
	case BuiltinProc_simd_lanes_eq:
	case BuiltinProc_simd_lanes_ne:
	case BuiltinProc_simd_lanes_lt:
	case BuiltinProc_simd_lanes_le:
	case BuiltinProc_simd_lanes_gt:
	case BuiltinProc_simd_lanes_ge:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			Type *et = nullptr;
			i64 count = 0;
			cgValue xb = cg_simd_base_ptr(p, x, &et, &count);
			cgValue yb = cg_simd_base_ptr(p, y, nullptr, nullptr);

			Type *rt = base_type(result_type);
			GB_ASSERT(rt->kind == Type_SimdVector);
			Type *rt_et = rt->SimdVector.elem;
			TB_DataType rdt = cg_data_type(rt_et);

			bool is_float  = is_type_float(et);
			bool is_signed = is_type_integer(et) && !is_type_unsigned(et);

			cgAddr res = cg_add_local(p, result_type, nullptr, true);
			cgValue rb = cg_value(cg_addr_get_ptr(p, res).node, alloc_type_pointer(rt_et));

			for (i64 i = 0; i < count; i++) {
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)i);
				TB_Node *a = cg_simd_load_lane(p, xb, et, idx);
				TB_Node *b = cg_simd_load_lane(p, yb, et, idx);
				TB_Node *cond = nullptr;
				if (is_float) {
					switch (id) {
					case BuiltinProc_simd_lanes_eq: {
						// feq = (a <= b) && (a >= b)
						TB_Node *le = tb_inst_cmp_fle(p->func, a, b);
						TB_Node *ge = tb_inst_cmp_fge(p->func, a, b);
						cond = tb_inst_and(p->func, le, ge);
						break;
					}
					case BuiltinProc_simd_lanes_ne: {
						TB_Node *le = tb_inst_cmp_fle(p->func, a, b);
						TB_Node *ge = tb_inst_cmp_fge(p->func, a, b);
						cond = tb_inst_not(p->func, tb_inst_and(p->func, le, ge));
						break;
					}
					case BuiltinProc_simd_lanes_lt: cond = tb_inst_cmp_flt(p->func, a, b); break;
					case BuiltinProc_simd_lanes_le: cond = tb_inst_cmp_fle(p->func, a, b); break;
					case BuiltinProc_simd_lanes_gt: cond = tb_inst_cmp_fgt(p->func, a, b); break;
					case BuiltinProc_simd_lanes_ge: cond = tb_inst_cmp_fge(p->func, a, b); break;
					}
				} else {
					switch (id) {
					case BuiltinProc_simd_lanes_eq: cond = tb_inst_cmp_eq(p->func, a, b); break;
					case BuiltinProc_simd_lanes_ne: cond = tb_inst_cmp_ne(p->func, a, b); break;
					case BuiltinProc_simd_lanes_lt: cond = tb_inst_cmp_ilt(p->func, a, b, is_signed); break;
					case BuiltinProc_simd_lanes_le: cond = tb_inst_cmp_ile(p->func, a, b, is_signed); break;
					case BuiltinProc_simd_lanes_gt: cond = tb_inst_cmp_igt(p->func, a, b, is_signed); break;
					case BuiltinProc_simd_lanes_ge: cond = tb_inst_cmp_ige(p->func, a, b, is_signed); break;
					}
				}
				GB_ASSERT(cond != nullptr);
				// sext the i1 compare result: true -> all ones, false -> 0
				// NOTE(odin-tilde): x64 isel has no sign-ext-to-I8 pattern,
				// so widen via I32 and truncate back down for byte masks
				TB_Node *mask = nullptr;
				if (rdt.type == TB_TAG_I8) {
					mask = tb_inst_trunc(p->func, tb_inst_sxt(p->func, cond, TB_TYPE_I32), rdt);
				} else {
					mask = tb_inst_sxt(p->func, cond, rdt);
				}
				cgValue ep = cg_emit_ptr_offset(p, rb, cg_const_int(p, t_int, i));
				cg_emit_store(p, ep, cg_value(mask, rt_et));
			}
			return cg_addr_load(p, res);
		}

	case BuiltinProc_simd_select:
		{
			cgValue cv = cg_build_expr(p, ce->args[0]);
			cgValue x  = cg_build_expr(p, ce->args[1]);
			cgValue y  = cg_build_expr(p, ce->args[2]);
			Type *cond_et = nullptr;
			i64 count = 0;
			cgValue cb = cg_simd_base_ptr(p, cv, &cond_et, &count);
			Type *et = nullptr;
			cgValue xb = cg_simd_base_ptr(p, x, &et, nullptr);
			cgValue yb = cg_simd_base_ptr(p, y, nullptr, nullptr);
			TB_DataType edt = cg_data_type(et);
			TB_DataType cdt = cg_data_type(cond_et);

			cgAddr res = cg_add_local(p, result_type, nullptr, true);
			cgValue rb = cg_value(cg_addr_get_ptr(p, res).node, alloc_type_pointer(et));

			for (i64 i = 0; i < count; i++) {
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)i);
				TB_Node *c = cg_simd_load_lane(p, cb, cond_et, idx);
				TB_Node *is_true = tb_inst_cmp_ne(p->func, c, tb_inst_uint(p->func, cdt, 0));
				TB_Node *a = cg_simd_load_lane(p, xb, et, idx);
				TB_Node *b = cg_simd_load_lane(p, yb, et, idx);
				TB_Node *r = tb_inst_select(p->func, is_true, a, b);
				gb_unused(edt);
				cgValue ep = cg_emit_ptr_offset(p, rb, cg_const_int(p, t_int, i));
				cg_emit_store(p, ep, cg_value(r, et));
			}
			return cg_addr_load(p, res);
		}

	case BuiltinProc_simd_reduce_or:
	case BuiltinProc_simd_reduce_and:
	case BuiltinProc_simd_reduce_xor:
	case BuiltinProc_simd_reduce_add_bisect:
	case BuiltinProc_simd_reduce_add_ordered:
	case BuiltinProc_simd_reduce_add_pairs:
	case BuiltinProc_simd_reduce_mul_bisect:
	case BuiltinProc_simd_reduce_mul_ordered:
	case BuiltinProc_simd_reduce_mul_pairs:
	case BuiltinProc_simd_reduce_min:
	case BuiltinProc_simd_reduce_max:
	case BuiltinProc_simd_reduce_any:
	case BuiltinProc_simd_reduce_all:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			Type *et = nullptr;
			i64 count = 0;
			cgValue xb = cg_simd_base_ptr(p, x, &et, &count);

			bool is_float  = is_type_float(et);
			bool is_signed = is_type_integer(et) && !is_type_unsigned(et);

			TB_Node *acc = cg_simd_load_lane(p, xb, et, tb_inst_uint(p->func, TB_TYPE_INT, 0));
			for (i64 i = 1; i < count; i++) {
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)i);
				TB_Node *lane = cg_simd_load_lane(p, xb, et, idx);
				switch (id) {
				case BuiltinProc_simd_reduce_or:
				case BuiltinProc_simd_reduce_any:
					acc = tb_inst_or(p->func, acc, lane);
					break;
				case BuiltinProc_simd_reduce_and:
				case BuiltinProc_simd_reduce_all:
					acc = tb_inst_and(p->func, acc, lane);
					break;
				case BuiltinProc_simd_reduce_xor:
					acc = tb_inst_xor(p->func, acc, lane);
					break;
				case BuiltinProc_simd_reduce_add_bisect:
				case BuiltinProc_simd_reduce_add_ordered:
				case BuiltinProc_simd_reduce_add_pairs:
					acc = is_float ? tb_inst_fadd(p->func, acc, lane)
					               : tb_inst_add(p->func, acc, lane, TB_ARITHMATIC_NONE);
					break;
				case BuiltinProc_simd_reduce_mul_bisect:
				case BuiltinProc_simd_reduce_mul_ordered:
				case BuiltinProc_simd_reduce_mul_pairs:
					acc = is_float ? tb_inst_fmul(p->func, acc, lane)
					               : tb_inst_mul(p->func, acc, lane, TB_ARITHMATIC_NONE);
					break;
				case BuiltinProc_simd_reduce_min: {
					TB_Node *cond = is_float ? tb_inst_cmp_flt(p->func, lane, acc)
					                         : tb_inst_cmp_ilt(p->func, lane, acc, is_signed);
					acc = tb_inst_select(p->func, cond, lane, acc);
					break;
				}
				case BuiltinProc_simd_reduce_max: {
					TB_Node *cond = is_float ? tb_inst_cmp_fgt(p->func, lane, acc)
					                         : tb_inst_cmp_igt(p->func, lane, acc, is_signed);
					acc = tb_inst_select(p->func, cond, lane, acc);
					break;
				}
				}
			}
			if (id == BuiltinProc_simd_reduce_any || id == BuiltinProc_simd_reduce_all) {
				TB_DataType edt = cg_data_type(et);
				acc = tb_inst_cmp_ne(p->func, acc, tb_inst_uint(p->func, edt, 0));
			}
			return cg_value(acc, result_type);
		}

	case BuiltinProc_simd_extract:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue index = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t_int);
			index = cg_flatten_value(p, index);
			Type *et = nullptr;
			cgValue xb = cg_simd_base_ptr(p, x, &et, nullptr);
			cgValue ep = cg_emit_ptr_offset(p, xb, index);
			TB_DataType edt = cg_data_type(et);
			TB_CharUnits align = cast(TB_CharUnits)type_align_of(et);
			return cg_value(tb_inst_load(p->func, edt, ep.node, align, false), result_type);
		}

	case BuiltinProc_simd_replace:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue index = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t_int);
			index = cg_flatten_value(p, index);
			cgValue v = cg_build_expr(p, ce->args[2]);
			Type *et = nullptr;
			cgValue xb = cg_simd_base_ptr(p, x, &et, nullptr);
			v = cg_emit_conv(p, v, et);
			v = cg_flatten_value(p, v);

			cgAddr res = cg_add_local(p, result_type, nullptr, false);
			cgValue rp = cg_addr_get_ptr(p, res);
			cgValue sz = cg_const_int(p, t_int, type_size_of(result_type));
			tb_inst_memcpy(p->func, rp.node, xb.node, sz.node, cast(TB_CharUnits)type_align_of(result_type));

			cgValue rb = cg_value(rp.node, alloc_type_pointer(et));
			cgValue ep = cg_emit_ptr_offset(p, rb, index);
			cg_emit_store(p, ep, v);
			return cg_addr_load(p, res);
		}

	case BuiltinProc_simd_abs:
	case BuiltinProc_simd_min:
	case BuiltinProc_simd_max:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = {};
			Type *et = nullptr;
			i64 count = 0;
			cgValue xb = cg_simd_base_ptr(p, x, &et, &count);
			cgValue yb = {};
			if (id != BuiltinProc_simd_abs) {
				y = cg_build_expr(p, ce->args[1]);
				yb = cg_simd_base_ptr(p, y, nullptr, nullptr);
			}
			bool is_float  = is_type_float(et);
			bool is_signed = is_type_integer(et) && !is_type_unsigned(et);
			TB_DataType edt = cg_data_type(et);

			cgAddr res = cg_add_local(p, result_type, nullptr, true);
			cgValue rb = cg_value(cg_addr_get_ptr(p, res).node, alloc_type_pointer(et));

			for (i64 i = 0; i < count; i++) {
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)i);
				TB_Node *a = cg_simd_load_lane(p, xb, et, idx);
				TB_Node *r = nullptr;
				if (id == BuiltinProc_simd_abs) {
					TB_Node *neg = is_float ? tb_inst_neg(p->func, a)
					                        : tb_inst_sub(p->func, tb_inst_uint(p->func, edt, 0), a, TB_ARITHMATIC_NONE);
					TB_Node *cond = is_float ? tb_inst_cmp_fgt(p->func, a, neg)
					                         : tb_inst_cmp_igt(p->func, a, neg, is_signed);
					r = tb_inst_select(p->func, cond, a, neg);
				} else {
					TB_Node *b = cg_simd_load_lane(p, yb, et, idx);
					TB_Node *cond = nullptr;
					if (id == BuiltinProc_simd_min) {
						cond = is_float ? tb_inst_cmp_flt(p->func, a, b)
						                : tb_inst_cmp_ilt(p->func, a, b, is_signed);
					} else {
						cond = is_float ? tb_inst_cmp_fgt(p->func, a, b)
						                : tb_inst_cmp_igt(p->func, a, b, is_signed);
					}
					r = tb_inst_select(p->func, cond, a, b);
				}
				cgValue ep = cg_emit_ptr_offset(p, rb, cg_const_int(p, t_int, i));
				cg_emit_store(p, ep, cg_value(r, et));
			}
			return cg_addr_load(p, res);
		}

	// PATCH(odin-tilde): per-lane arithmetic/simd shifts (simd_add family)
	case BuiltinProc_simd_add:
	case BuiltinProc_simd_sub:
	case BuiltinProc_simd_mul:
	case BuiltinProc_simd_div:
	case BuiltinProc_simd_shl:
	case BuiltinProc_simd_shr:
	case BuiltinProc_simd_shl_masked:
	case BuiltinProc_simd_shr_masked:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			Type *et = nullptr;
			i64 count = 0;
			cgValue xb = cg_simd_base_ptr(p, x, &et, &count);
			cgValue yb = cg_simd_base_ptr(p, y, nullptr, nullptr);

			bool is_float  = is_type_float(et);
			bool is_signed = is_type_integer(et) && !is_type_unsigned(et);
			TB_DataType edt = cg_data_type(et);
			i64 bit_width = 8 * type_size_of(et);

			cgAddr res = cg_add_local(p, result_type, nullptr, true);
			cgValue rb = cg_value(cg_addr_get_ptr(p, res).node, alloc_type_pointer(et));

			for (i64 i = 0; i < count; i++) {
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)i);
				TB_Node *a = cg_simd_load_lane(p, xb, et, idx);
				TB_Node *b = cg_simd_load_lane(p, yb, et, idx);
				TB_Node *r = nullptr;
				switch (id) {
				case BuiltinProc_simd_add:
					r = is_float ? tb_inst_fadd(p->func, a, b)
					             : tb_inst_add(p->func, a, b, TB_ARITHMATIC_NONE);
					break;
				case BuiltinProc_simd_sub:
					r = is_float ? tb_inst_fsub(p->func, a, b)
					             : tb_inst_sub(p->func, a, b, TB_ARITHMATIC_NONE);
					break;
				case BuiltinProc_simd_mul:
					r = is_float ? tb_inst_fmul(p->func, a, b)
					             : tb_inst_mul(p->func, a, b, TB_ARITHMATIC_NONE);
					break;
				case BuiltinProc_simd_div:
					r = is_float ? tb_inst_fdiv(p->func, a, b)
					             : tb_inst_div(p->func, a, b, is_signed);
					break;
				case BuiltinProc_simd_shl:
				case BuiltinProc_simd_shl_masked: {
					TB_Node *amt = b;
					if (id == BuiltinProc_simd_shl_masked) {
						amt = tb_inst_and(p->func, amt, tb_inst_uint(p->func, edt, cast(u64)(bit_width - 1)));
					}
					r = tb_inst_shl(p->func, a, amt, TB_ARITHMATIC_NONE);
					break;
				}
				case BuiltinProc_simd_shr:
				case BuiltinProc_simd_shr_masked: {
					TB_Node *amt = b;
					if (id == BuiltinProc_simd_shr_masked) {
						amt = tb_inst_and(p->func, amt, tb_inst_uint(p->func, edt, cast(u64)(bit_width - 1)));
					}
					r = is_signed ? tb_inst_sar(p->func, a, amt)
					              : tb_inst_shr(p->func, a, amt);
					break;
				}
				}
				GB_ASSERT(r != nullptr);
				cgValue ep = cg_emit_ptr_offset(p, rb, cg_const_int(p, t_int, i));
				cg_emit_store(p, ep, cg_value(r, et));
			}
			return cg_addr_load(p, res);
		}

	case BuiltinProc_simd_neg:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			Type *et = nullptr;
			i64 count = 0;
			cgValue xb = cg_simd_base_ptr(p, x, &et, &count);
			bool is_float = is_type_float(et);
			TB_DataType edt = cg_data_type(et);

			cgAddr res = cg_add_local(p, result_type, nullptr, true);
			cgValue rb = cg_value(cg_addr_get_ptr(p, res).node, alloc_type_pointer(et));

			for (i64 i = 0; i < count; i++) {
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)i);
				TB_Node *a = cg_simd_load_lane(p, xb, et, idx);
				TB_Node *r = is_float ? tb_inst_neg(p->func, a)
				                      : tb_inst_sub(p->func, tb_inst_uint(p->func, edt, 0), a, TB_ARITHMATIC_NONE);
				cgValue ep = cg_emit_ptr_offset(p, rb, cg_const_int(p, t_int, i));
				cg_emit_store(p, ep, cg_value(r, et));
			}
			return cg_addr_load(p, res);
		}

	// PATCH(odin-tilde): integer bitwise lane ops
	case BuiltinProc_simd_bit_and:
	case BuiltinProc_simd_bit_or:
	case BuiltinProc_simd_bit_xor:
	case BuiltinProc_simd_bit_and_not:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			Type *et = nullptr;
			i64 count = 0;
			cgValue xb = cg_simd_base_ptr(p, x, &et, &count);
			cgValue yb = cg_simd_base_ptr(p, y, nullptr, nullptr);

			cgAddr res = cg_add_local(p, result_type, nullptr, true);
			cgValue rb = cg_value(cg_addr_get_ptr(p, res).node, alloc_type_pointer(et));

			for (i64 i = 0; i < count; i++) {
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)i);
				TB_Node *a = cg_simd_load_lane(p, xb, et, idx);
				TB_Node *b = cg_simd_load_lane(p, yb, et, idx);
				TB_Node *r = nullptr;
				switch (id) {
				case BuiltinProc_simd_bit_and:     r = tb_inst_and(p->func, a, b); break;
				case BuiltinProc_simd_bit_or:      r = tb_inst_or (p->func, a, b); break;
				case BuiltinProc_simd_bit_xor:     r = tb_inst_xor(p->func, a, b); break;
				case BuiltinProc_simd_bit_and_not: r = tb_inst_and(p->func, tb_inst_not(p->func, a), b); break;
				}
				GB_ASSERT(r != nullptr);
				cgValue ep = cg_emit_ptr_offset(p, rb, cg_const_int(p, t_int, i));
				cg_emit_store(p, ep, cg_value(r, et));
			}
			return cg_addr_load(p, res);
		}

	// PATCH(odin-tilde): compile-time shuffle (indices are constant args 2..)
	case BuiltinProc_simd_shuffle:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			Type *et = nullptr;
			i64 count = 0;
			cgValue xb = cg_simd_base_ptr(p, x, &et, &count);
			cgValue yb = cg_simd_base_ptr(p, y, nullptr, nullptr);

			i64 indices_count = ce->args.count - 2;
			cgAddr res = cg_add_local(p, result_type, nullptr, true);
			cgValue rb = cg_value(cg_addr_get_ptr(p, res).node, alloc_type_pointer(et));

			for (i64 i = 0; i < indices_count; i++) {
				TypeAndValue tav = type_and_value_of_expr(ce->args[i+2]);
				GB_ASSERT(tav.value.kind == ExactValue_Integer);
				i64 src = exact_value_to_i64(tav.value);
				cgValue base = xb;
				if (src >= count) {
					base = yb;
					src -= count;
				}
				TB_Node *idx = tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)src);
				TB_Node *lane = cg_simd_load_lane(p, base, et, idx);
				cgValue ep = cg_emit_ptr_offset(p, rb, cg_const_int(p, t_int, i));
				cg_emit_store(p, ep, cg_value(lane, et));
			}
			return cg_addr_load(p, res);
		}
	}

	GB_PANIC("TODO(bill): unhandled #simd builtin %s for tilde backend", builtin_procs[id].name);
	return {};
}




gb_internal cgValue cg_build_builtin(cgProcedure *p, BuiltinProcId id, Ast *expr) {
	ast_node(ce, CallExpr, expr);

	if (BuiltinProc__simd_begin < id && id < BuiltinProc__simd_end) {
		return cg_build_builtin_simd_proc(p, expr, id);
	}

	String builtin_name = builtin_procs[id].name;

	switch (id) {
	case BuiltinProc_DIRECTIVE: {
		ast_node(bd, BasicDirective, ce->proc);
		String name = bd->name.string;
		GB_ASSERT(name == "location");
		String procedure = p->entity->token.string;
		TokenPos pos = ast_token(ce->proc).pos;
		if (ce->args.count > 0) {
			Ast *ident = unselector_expr(ce->args[0]);
			GB_ASSERT(ident->kind == Ast_Ident);
			Entity *e = entity_of_node(ident);
			GB_ASSERT(e != nullptr);

			DeclInfo *decl = e->parent_proc_decl.load();
			Entity *decl_entity = decl ? decl->entity.load() : nullptr;
			if (decl_entity != nullptr) {
				procedure = decl_entity->token.string;
			} else {
				procedure = str_lit("");
			}
			pos = e->token.pos;

		}
		return cg_emit_source_code_location_as_global(p, procedure, pos);
	} break;

	case BuiltinProc_len: {
		cgValue v = cg_build_expr(p, ce->args[0]);
		Type *t = base_type(v.type);
		if (is_type_pointer(t)) {
			// IMPORTANT TODO(bill): Should there be a nil pointer check?
			v = cg_emit_load(p, v);
			t = type_deref(t);
		}
		return cg_builtin_len(p, v);
	}

	case BuiltinProc_cap: {
		cgValue v = cg_build_expr(p, ce->args[0]);
		Type *t = base_type(v.type);
		if (is_type_pointer(t)) {
			// IMPORTANT TODO(bill): Should there be a nil pointer check?
			v = cg_emit_load(p, v);
			t = type_deref(t);
		}
		return cg_builtin_cap(p, v);
	}

	case BuiltinProc_raw_data:
		{
			cgValue v = cg_build_expr(p, ce->args[0]);
			return cg_builtin_raw_data(p, v);
		}

	case BuiltinProc_real:
	case BuiltinProc_imag:
	case BuiltinProc_jmag:
	case BuiltinProc_kmag: {
		// NOTE(new-tb): complex & quaternion are memory aggregates in this
		// backend; extract the element through its address.
		// @QuaternionLayout: i,j,k,w at indices 0,1,2,3
		cgValue val = cg_build_expr(p, ce->args[0]);
		Type *t = type_of_expr(expr);
		isize index = 0;
		if (is_type_complex(val.type)) {
			index = (id == BuiltinProc_imag) ? 1 : 0;
		} else if (is_type_quaternion(val.type)) {
			switch (id) {
			case BuiltinProc_imag: index = 0; break;
			case BuiltinProc_jmag: index = 1; break;
			case BuiltinProc_kmag: index = 2; break;
			default:             index = 3; break; // real
			}
		} else {
			GB_PANIC("invalid type for %.*s", LIT(builtin_name));
		}
		cgValue ptr = cg_address_from_load_or_generate_local(p, val);
		cgValue ep  = cg_emit_struct_ep(p, ptr, index);
		cgValue ev  = cg_emit_load(p, ep);
		return cg_emit_conv(p, ev, t);
	}

	case BuiltinProc_hadamard_product: {
		// NOTE(odin-tilde): mirrors lb — component-wise multiply.
		cgValue a = cg_build_expr(p, ce->args[0]);
		cgValue b = cg_build_expr(p, ce->args[1]);
		Type *t = type_of_expr(expr);
		if (is_type_array(t)) {
			return cg_emit_arith(p, Token_Mul, a, b, t);
		}
		GB_ASSERT(is_type_matrix(t));
		return cg_emit_arith_matrix(p, Token_Mul, a, b, t, true);
	}

	case BuiltinProc_matrix_flatten: {
		// NOTE(odin-tilde): mirrors lb_emit_matrix_flatten — a matrix is
		// already a flat row/column-major array in memory, so this is a
		// raw copy into an array local.
		cgValue m = cg_build_expr(p, ce->args[0]);
		Type *t = type_of_expr(expr);
		if (is_type_array(m.type)) {
			m.type = t;
			return m;
		}
		GB_ASSERT(is_type_matrix(base_type(m.type)));
		GB_ASSERT(type_size_of(t) == type_size_of(m.type));
		cgAddr res = cg_add_local(p, t, nullptr, false);
		cgValue m_ptr = cg_address_from_load_or_generate_local(p, m);
		i64 size = type_size_of(t);
		tb_inst_memcpy(p->func, res.addr.node, m_ptr.node,
		               tb_inst_uint(p->func, TB_TYPE_INT, cast(u64)size),
		               cast(TB_CharUnits)type_align_of(t));
		return cg_addr_load(p, res);
	}

	case BuiltinProc_swizzle: {
		// NOTE(odin-tilde): fixed-array swizzle; materialize the source
		// and copy elements one by one into a result local. SIMD-vector
		// swizzle is still unimplemented (KNOWN_ISSUES.md).
		if (is_type_simd_vector(type_of_expr(expr))) {
			GB_PANIC("TODO(bill): builtin procs %d %.*s (simd)", id, LIT(builtin_name));
		}
		Type *t = type_of_expr(expr);
		cgValue arr = cg_build_expr(p, ce->args[0]);
		cgValue arr_ptr = cg_address_from_load_or_generate_local(p, arr);
		cgAddr dst_addr = cg_add_local(p, t, nullptr, false);
		cgValue dst = cg_addr_get_ptr(p, dst_addr);
		for (isize i = 1; i < ce->args.count; i++) {
			TypeAndValue itav = type_and_value_of_expr(ce->args[i]);
			GB_ASSERT(itav.value.kind == ExactValue_Integer);
			i64 index = exact_value_to_i64(itav.value);
			cgValue ev = cg_emit_load(p, cg_emit_array_epi(p, arr_ptr, index));
			cg_emit_store(p, cg_emit_array_epi(p, dst, i-1), ev);
		}
		return cg_addr_load(p, dst_addr);
	}

	case BuiltinProc_complex: {
		// NOTE(odin-tilde): mirrors the LLVM backend; build the 2-field
		// aggregate in a local.
		cgValue real = cg_build_expr(p, ce->args[0]);
		cgValue imag = cg_build_expr(p, ce->args[1]);
		Type *t = type_of_expr(expr);
		Type *ft = base_complex_elem_type(t);
		real = cg_emit_conv(p, real, ft);
		imag = cg_emit_conv(p, imag, ft);

		cgAddr dst_addr = cg_add_local(p, t, nullptr, false);
		cgValue dst = cg_addr_get_ptr(p, dst_addr);
		cg_emit_store(p, cg_emit_struct_ep(p, dst, 0), real);
		cg_emit_store(p, cg_emit_struct_ep(p, dst, 1), imag);
		return cg_addr_load(p, dst_addr);
	}

	case BuiltinProc_quaternion: {
		// NOTE(odin-tilde): mirrors the LLVM backend.
		// @QuaternionLayout: i,j,k,w at indices 0,1,2,3
		Type *t = type_of_expr(expr);
		cgValue xyzw[4] = {};
		for (i32 i = 0; i < 4; i++) {
			ast_node(f, FieldValue, ce->args[i]);
			GB_ASSERT(f->field->kind == Ast_Ident);
			String name = f->field->Ident.token.string;
			i32 index = -1;
			if (name == "x" || name == "imag") {
				index = 0;
			} else if (name == "y" || name == "jmag") {
				index = 1;
			} else if (name == "z" || name == "kmag") {
				index = 2;
			} else if (name == "w" || name == "real") {
				index = 3;
			}
			GB_ASSERT(index >= 0);
			xyzw[index] = cg_build_expr(p, f->value);
		}

		Type *ft = base_complex_elem_type(t);
		cgAddr dst_addr = cg_add_local(p, t, nullptr, false);
		cgValue dst = cg_addr_get_ptr(p, dst_addr);
		for (i32 i = 0; i < 4; i++) {
			xyzw[i] = cg_emit_conv(p, xyzw[i], ft);
			cg_emit_store(p, cg_emit_struct_ep(p, dst, i), xyzw[i]);
		}
		return cg_addr_load(p, dst_addr);
	}

	case BuiltinProc_conj: {
		// NOTE(odin-tilde): complex negates imag (index 1); quaternion
		// negates i,j,k (indices 0,1,2) and keeps w (index 3).
		cgValue val = cg_build_expr(p, ce->args[0]);
		Type *t = type_of_expr(expr);
		if (is_type_complex(val.type) || is_type_quaternion(val.type)) {
			isize n = is_type_complex(val.type) ? 2 : 4;
			cgValue ptr = cg_address_from_load_or_generate_local(p, val);
			cgAddr dst_addr = cg_add_local(p, t, nullptr, false);
			cgValue dst = cg_addr_get_ptr(p, dst_addr);
			for (isize i = 0; i < n; i++) {
				cgValue ev = cg_emit_load(p, cg_emit_struct_ep(p, ptr, i));
				bool negate = is_type_complex(val.type) ? (i == 1) : (i != 3);
				if (negate) {
					ev = cg_emit_unary_arith(p, Token_Sub, ev, ev.type);
				}
				cg_emit_store(p, cg_emit_struct_ep(p, dst, i), ev);
			}
			return cg_addr_load(p, dst_addr);
		}
		GB_PANIC("TODO(bill): conj for %s", type_to_string(val.type));
	}

	case BuiltinProc_soa_zip: {
		// NOTE(odin-tilde): mirrors lb_soa_zip
		GB_ASSERT(ce->args.count > 0);

		auto slices = slice_make<cgValue>(temporary_allocator(), ce->args.count);
		for_array(i, slices) {
			Ast *arg = ce->args[i];
			if (arg->kind == Ast_FieldValue) {
				arg = arg->FieldValue.value;
			}
			slices[i] = cg_build_expr(p, arg);
		}

		cgValue len = cg_builtin_len(p, slices[0]);
		for (isize i = 1; i < slices.count; i++) {
			cgValue other_len = cg_builtin_len(p, slices[i]);
			len = cg_builtin_min(p, t_int, len, other_len);
		}

		Type *t = type_of_expr(expr);
		GB_ASSERT(is_type_soa_struct(t));
		cgAddr res = cg_add_local(p, t, nullptr, true);
		cgValue rp = cg_addr_get_ptr(p, res);
		for_array(i, slices) {
			cgValue src = cg_builtin_raw_data(p, slices[i]);
			src = cg_emit_conv(p, src, alloc_type_pointer_to_multi_pointer(src.type));
			cgValue dst = cg_emit_struct_ep(p, rp, cast(i32)i);
			cg_emit_store(p, dst, src);
		}
		cgValue len_dst = cg_emit_struct_ep(p, rp, cast(i32)slices.count);
		cg_emit_store(p, len_dst, len);

		return cg_addr_load(p, res);
	}

	case BuiltinProc_soa_unzip: {
		// NOTE(odin-tilde): mirrors lb_soa_unzip
		GB_ASSERT(ce->args.count == 1);

		cgValue arg = cg_build_expr(p, ce->args[0]);
		Type *t = base_type(arg.type);
		GB_ASSERT(is_type_soa_struct(t) && t->Struct.soa_kind == StructSoa_Slice);

		cgValue len = cg_builtin_len(p, arg);

		Type *rt = type_of_expr(expr);
		cgAddr res = cg_add_local(p, rt, nullptr, true);
		if (is_type_tuple(rt)) {
			cgValue rp = cg_addr_get_ptr(p, res);
			for (i32 i = 0; i < cast(i32)(t->Struct.fields.count-1); i++) {
				cgValue ptr = cg_emit_struct_ev(p, arg, i);
				cgAddr dst = cg_addr(cg_emit_struct_ep(p, rp, i));
				cg_fill_slice(p, dst, ptr, len);
			}
		} else {
			GB_ASSERT(is_type_slice(rt));
			cgValue ptr = cg_emit_struct_ev(p, arg, 0);
			cg_fill_slice(p, res, ptr, len);
		}

		return cg_addr_load(p, res);
	}

	case BuiltinProc_min:
		if (ce->args.count == 2) {
			Type *t = type_of_expr(expr);
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			return cg_builtin_min(p, t, x, y);
		} else {
			Type *t = type_of_expr(expr);
			cgValue x = cg_build_expr(p, ce->args[0]);
			for (isize i = 1; i < ce->args.count; i++) {
				cgValue y = cg_build_expr(p, ce->args[i]);
				x = cg_builtin_min(p, t, x, y);
			}
			return x;
		}
		break;
	case BuiltinProc_max:
		if (ce->args.count == 2) {
			Type *t = type_of_expr(expr);
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			return cg_builtin_max(p, t, x, y);
		} else {
			Type *t = type_of_expr(expr);
			cgValue x = cg_build_expr(p, ce->args[0]);
			for (isize i = 1; i < ce->args.count; i++) {
				cgValue y = cg_build_expr(p, ce->args[i]);
				x = cg_builtin_max(p, t, x, y);
			}
			return x;
		}
		break;

	case BuiltinProc_abs:
		{
			cgValue x = cg_build_expr(p, ce->args[0]);
			return cg_builtin_abs(p, x);
		}

	case BuiltinProc_sqrt:
		{
			// PATCH(odin-tilde): scalar/vector float sqrt via TB_FSQRT
			// (x64 sqrtsd/sqrtss), mirrors the LLVM backend's llvm.sqrt path.
			Type *type = type_of_expr(expr);
			cgValue x = cg_build_expr(p, ce->args[0]);
			x = cg_emit_conv(p, x, type);
			TB_Node *r = tb_inst_fsqrt(p->func, x.node);
			return cg_value(r, type);
		}

	case BuiltinProc_clamp:
		{
			cgValue x   = cg_build_expr(p, ce->args[0]);
			cgValue min = cg_build_expr(p, ce->args[1]);
			cgValue max = cg_build_expr(p, ce->args[2]);
			return cg_builtin_clamp(p, type_of_expr(expr), x, min, max);
		}

	case BuiltinProc_debug_trap:
		tb_inst_debugbreak(p->func);
		return {};
	case BuiltinProc_trap:
		tb_inst_trap(p->func);
		return {};

	case BuiltinProc_mem_zero:
		{
			cgValue ptr = cg_build_expr(p, ce->args[0]);
			cgValue len = cg_build_expr(p, ce->args[1]);
			return cg_builtin_mem_zero(p, ptr, len);
		}

	case BuiltinProc_mem_zero_volatile:
		{
			// NOTE(odin-tilde): volatile variant of mem_zero
			cgValue ptr = cg_build_expr(p, ce->args[0]);
			cgValue len = cg_build_expr(p, ce->args[1]);
			GB_ASSERT(ptr.kind == cgValue_Value);
			GB_ASSERT(len.kind == cgValue_Value);
			tb_builder_memzero(cg_tb_g(p->func), 0, true, ptr.node, len.node, 1, true);
			return ptr;
		}

	case BuiltinProc_mem_copy:
		{
			cgValue dst = cg_build_expr(p, ce->args[0]);
			cgValue src = cg_build_expr(p, ce->args[1]);
			cgValue len = cg_build_expr(p, ce->args[2]);
			return cg_builtin_mem_copy(p, dst, src, len);
		}

	case BuiltinProc_mem_copy_non_overlapping:
		{
			cgValue dst = cg_build_expr(p, ce->args[0]);
			cgValue src = cg_build_expr(p, ce->args[1]);
			cgValue len = cg_build_expr(p, ce->args[2]);
			return cg_builtin_mem_copy_non_overlapping(p, dst, src, len);
		}


	case BuiltinProc_unaligned_load:
		{
			cgValue src = cg_build_expr(p, ce->args[0]);
			src = cg_flatten_value(p, src);
			Type *t = type_deref(src.type);
			TB_DataType dt = cg_data_type(t);
			if (!TB_IS_VOID_TYPE(dt)) {
				// scalar: plain load at alignment 1
				TB_Node *n = tb_inst_load(p->func, dt, src.node, 1, false);
				return cg_value(n, t);
			}
			// aggregate: copy into a local
			cgAddr dst = cg_add_local(p, t, nullptr, false);
			cgValue sz = cg_const_int(p, t_int, type_size_of(t));
			tb_inst_memcpy(p->func, cg_addr_get_ptr(p, dst).node, src.node, sz.node, 1);
			return cg_addr_load(p, dst);
		}

	case BuiltinProc_unaligned_store:
		{
			cgValue dst = cg_build_expr(p, ce->args[0]);
			dst = cg_flatten_value(p, dst);
			Type *t = type_deref(dst.type);
			cgValue v = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t);
			TB_DataType dt = cg_data_type(t);
			if (!TB_IS_VOID_TYPE(dt)) {
				v = cg_flatten_value(p, v);
				tb_inst_store(p->func, dt, dst.node, v.node, 1, false);
			} else {
				cgValue src = cg_address_from_load_or_generate_local(p, v);
				cgValue sz = cg_const_int(p, t_int, type_size_of(t));
				tb_inst_memcpy(p->func, dst.node, src.node, sz.node, 1);
			}
			return {};
		}

	case BuiltinProc_volatile_load:
		{
			cgValue src = cg_build_expr(p, ce->args[0]);
			src = cg_flatten_value(p, src);
			Type *t = type_deref(src.type);
			TB_DataType dt = cg_data_type(t);
			if (!TB_IS_VOID_TYPE(dt)) {
				TB_Node *n = tb_inst_load(p->func, dt, src.node, cast(TB_CharUnits)type_align_of(t), true);
				return cg_value(n, t);
			}
			cgAddr dst = cg_add_local(p, t, nullptr, false);
			cgValue sz = cg_const_int(p, t_int, type_size_of(t));
			tb_inst_memcpy(p->func, cg_addr_get_ptr(p, dst).node, src.node, sz.node, 1);
			return cg_addr_load(p, dst);
		}

	case BuiltinProc_volatile_store:
		{
			cgValue dst = cg_build_expr(p, ce->args[0]);
			dst = cg_flatten_value(p, dst);
			Type *t = type_deref(dst.type);
			cgValue v = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t);
			TB_DataType dt = cg_data_type(t);
			if (!TB_IS_VOID_TYPE(dt)) {
				v = cg_flatten_value(p, v);
				tb_inst_store(p->func, dt, dst.node, v.node, cast(TB_CharUnits)type_align_of(t), true);
			} else {
				cgValue src = cg_address_from_load_or_generate_local(p, v);
				cgValue sz = cg_const_int(p, t_int, type_size_of(t));
				tb_inst_memcpy(p->func, dst.node, src.node, sz.node, 1);
			}
			return {};
		}

	// NOTE(odin-tilde): atomics lower to TB's atomic nodes (lock-prefixed
	// ops on x64). Fences have no TB node, so they become a LOCKed add of 0
	// to a dummy global — a full fence on x64.
	case BuiltinProc_atomic_thread_fence:
	case BuiltinProc_atomic_signal_fence:
		{
			if (p->module->fence_dummy == nullptr) {
				p->module->fence_dummy = tb_global_create(p->module->mod, -1, "__$fence_dummy", nullptr, TB_LINKAGE_PRIVATE);
				tb_global_set_storage(p->module->mod, tb_module_get_data(p->module->mod), p->module->fence_dummy, 8, 8, 1);
			}
			TB_Node *addr = tb_inst_get_symbol_address(p->func, cast(TB_Symbol *)p->module->fence_dummy);
			TB_Node *zero = tb_inst_uint(p->func, TB_TYPE_I64, 0);
			tb_builder_atomic_rmw(cg_tb_g(p->func), 0, TB_ATOMIC_ADD, addr, zero, TB_MEM_ORDER_SEQ_CST);
			return {};
		}

	// NOTE(odin-tilde): TB has no pause/spin-loop-hint node; cpu_relax is
	// only a performance hint, so a no-op is semantically correct.
	case BuiltinProc_cpu_relax:
		return {};

	case BuiltinProc_atomic_load:
	case BuiltinProc_atomic_load_explicit:
		{
			cgValue dst = cg_flatten_value(p, cg_build_expr(p, ce->args[0]));
			Type *t = type_deref(dst.type);
			TB_MemoryOrder order = TB_MEM_ORDER_SEQ_CST;
			if (id == BuiltinProc_atomic_load_explicit) {
				order = cg_tb_memory_order_from_odin(ce->args[1]);
			}
			TB_Node *n = tb_builder_atomic_load(cg_tb_g(p->func), 0, cg_data_type(t), dst.node, order);
			return cg_value(n, t);
		}

	case BuiltinProc_atomic_store:
	case BuiltinProc_atomic_store_explicit:
		{
			cgValue dst = cg_flatten_value(p, cg_build_expr(p, ce->args[0]));
			Type *t = type_deref(dst.type);
			cgValue v = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t);
			v = cg_flatten_value(p, v);
			TB_MemoryOrder order = TB_MEM_ORDER_SEQ_CST;
			if (id == BuiltinProc_atomic_store_explicit) {
				order = cg_tb_memory_order_from_odin(ce->args[2]);
			}
			tb_builder_atomic_store(cg_tb_g(p->func), 0, dst.node, v.node, order);
			return {};
		}

	case BuiltinProc_atomic_add:
	case BuiltinProc_atomic_sub:
	case BuiltinProc_atomic_and:
	case BuiltinProc_atomic_or:
	case BuiltinProc_atomic_xor:
	case BuiltinProc_atomic_exchange:
	case BuiltinProc_atomic_add_explicit:
	case BuiltinProc_atomic_sub_explicit:
	case BuiltinProc_atomic_and_explicit:
	case BuiltinProc_atomic_or_explicit:
	case BuiltinProc_atomic_xor_explicit:
	case BuiltinProc_atomic_exchange_explicit:
		{
			cgValue dst = cg_flatten_value(p, cg_build_expr(p, ce->args[0]));
			Type *t = type_deref(dst.type);
			cgValue v = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t);
			v = cg_flatten_value(p, v);

			int op = TB_ATOMIC_ADD;
			bool negate = false;
			bool explicit_order = false;
			switch (id) {
			case BuiltinProc_atomic_add:               op = TB_ATOMIC_ADD;  break;
			case BuiltinProc_atomic_sub:               op = TB_ATOMIC_ADD;  negate = true; break;
			case BuiltinProc_atomic_and:               op = TB_ATOMIC_AND;  break;
			case BuiltinProc_atomic_or:                op = TB_ATOMIC_OR;   break;
			case BuiltinProc_atomic_xor:               op = TB_ATOMIC_XOR;  break;
			case BuiltinProc_atomic_exchange:          op = TB_ATOMIC_XCHG; break;
			case BuiltinProc_atomic_add_explicit:      op = TB_ATOMIC_ADD;  explicit_order = true; break;
			case BuiltinProc_atomic_sub_explicit:      op = TB_ATOMIC_ADD;  negate = true; explicit_order = true; break;
			case BuiltinProc_atomic_and_explicit:      op = TB_ATOMIC_AND;  explicit_order = true; break;
			case BuiltinProc_atomic_or_explicit:       op = TB_ATOMIC_OR;   explicit_order = true; break;
			case BuiltinProc_atomic_xor_explicit:      op = TB_ATOMIC_XOR;  explicit_order = true; break;
			case BuiltinProc_atomic_exchange_explicit: op = TB_ATOMIC_XCHG; explicit_order = true; break;
			}

			TB_MemoryOrder order = TB_MEM_ORDER_SEQ_CST;
			if (explicit_order) {
				order = cg_tb_memory_order_from_odin(ce->args[2]);
			}

			if (negate) {
				// NOTE(odin-tilde): TB has no atomic sub; add of the
				// negated value is equivalent.
				TB_DataType dt = cg_data_type(t);
				TB_Node *zero = tb_inst_uint(p->func, dt, 0);
				v.node = tb_inst_sub(p->func, zero, v.node, TB_ARITHMATIC_NONE);
			}

			TB_Node *n = tb_builder_atomic_rmw(cg_tb_g(p->func), 0, op, dst.node, v.node, order);
			return cg_value(n, t);
		}

	// NOTE(odin-tilde): cxchg lowers to TB_ATOMIC_CAS (lock cmpxchg on x64).
	// weak/strong are identical on x64; the bool is old == expected.
	case BuiltinProc_atomic_compare_exchange_strong:
	case BuiltinProc_atomic_compare_exchange_weak:
	case BuiltinProc_atomic_compare_exchange_strong_explicit:
	case BuiltinProc_atomic_compare_exchange_weak_explicit:
		{
			cgValue dst = cg_flatten_value(p, cg_build_expr(p, ce->args[0]));
			Type *elem = type_deref(dst.type);
			cgValue old_v = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), elem);
			cgValue new_v = cg_emit_conv(p, cg_build_expr(p, ce->args[2]), elem);
			old_v = cg_flatten_value(p, old_v);
			new_v = cg_flatten_value(p, new_v);

			TB_MemoryOrder order = TB_MEM_ORDER_SEQ_CST;
			if (id == BuiltinProc_atomic_compare_exchange_strong_explicit ||
			    id == BuiltinProc_atomic_compare_exchange_weak_explicit) {
				order = cg_tb_memory_order_from_odin(ce->args[3]);
			}

			TB_Node *old_n = tb_builder_atomic_cas(cg_tb_g(p->func), 0, dst.node, old_v.node, new_v.node, order);

			Type *res_type = type_of_expr(expr);
			// NOTE(odin-tilde): cxchg is #optional_ok; when the bool is
			// dropped the result type is the plain element type.
			if (res_type->kind == Type_Tuple) {
				GB_ASSERT(res_type->Tuple.variables.count == 2);
				Type *ok_type = res_type->Tuple.variables[1]->type;
				TB_Node *ok_n = tb_inst_cmp_eq(p->func, old_n, old_v.node);
				return cg_value_multi2(cg_value(old_n, elem), cg_value(ok_n, ok_type), res_type);
			}
			return cg_value(old_n, elem);
		}

	case BuiltinProc_overflow_add:
	case BuiltinProc_overflow_sub:
		{
			// NOTE(odin-tilde): real carry/overflow detection (the old stub
			// always returned false, which broke math/bits add/sub carry and
			// sent core:math/big into an infinite Rabin-Miller loop).
			Type *res_type = type_of_expr(expr);
			GB_ASSERT(res_type->kind == Type_Tuple);
			GB_ASSERT(res_type->Tuple.variables.count == 2);
			Type *type = res_type->Tuple.variables[0]->type;
			Type *ok_type = res_type->Tuple.variables[1]->type;
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			x = cg_flatten_value(p, cg_emit_conv(p, x, type));
			y = cg_flatten_value(p, cg_emit_conv(p, y, type));

			TokenKind arith = (id == BuiltinProc_overflow_add) ? Token_Add : Token_Sub;
			cgValue res = cg_emit_arith(p, arith, x, y, type);

			cgValue ok = {};
			if (is_type_unsigned(type)) {
				// add: carry = res < x (unsigned); sub: borrow = x < y
				cgValue a = (id == BuiltinProc_overflow_add) ? res : x;
				cgValue b = (id == BuiltinProc_overflow_add) ? x   : y;
				ok = cg_emit_comp(p, Token_Lt, a, b);
			} else {
				// signed overflow: add -> ((x^res) & (y^res)) < 0
				//                  sub -> ((x^y) & (x^res)) < 0
				cgValue s1 = cg_emit_arith(p, Token_Xor, x, res, type);
				cgValue s2 = (id == BuiltinProc_overflow_add)
					? cg_emit_arith(p, Token_Xor, y, res, type)
					: cg_emit_arith(p, Token_Xor, x, y,   type);
				cgValue both = cg_emit_arith(p, Token_And, s1, s2, type);
				ok = cg_emit_comp(p, Token_Lt, both, cg_const_int(p, type, 0));
			}
			ok = cg_emit_conv(p, ok, ok_type);

			return cg_value_multi2(res, ok, res_type);
		}

	case BuiltinProc_overflow_mul:
		{
			Type *res_type = type_of_expr(expr);
			GB_ASSERT(res_type->kind == Type_Tuple);
			GB_ASSERT(res_type->Tuple.variables.count == 2);
			Type *type = res_type->Tuple.variables[0]->type;
			Type *ok_type = res_type->Tuple.variables[1]->type;
			cgValue x = cg_build_expr(p, ce->args[0]);
			cgValue y = cg_build_expr(p, ce->args[1]);
			x = cg_flatten_value(p, cg_emit_conv(p, x, type));
			y = cg_flatten_value(p, cg_emit_conv(p, y, type));

			i64 sz = type_size_of(type);
			bool sign = !is_type_unsigned(type);
			cgValue res = {};
			cgValue ok = {};
			if (sz < 8) {
				// NOTE(odin-tilde): widen into (u)i64, multiply, then check the
				// high bits (u64 multiply of two 32-bit values cannot overflow)
				Type *wide = sign ? t_i64 : t_u64;
				cgValue xw = cg_emit_conv(p, x, wide);
				cgValue yw = cg_emit_conv(p, y, wide);
				cgValue prod = cg_emit_arith(p, Token_Mul, xw, yw, wide);
				res = cg_emit_conv(p, prod, type);
				cgValue back = cg_emit_conv(p, res, wide);
				ok = cg_emit_comp(p, Token_NotEq, prod, back);
			} else {
				GB_ASSERT(cg_tb_builder != nullptr);
				cgValue xu = cg_emit_transmute(p, x, t_u64);
				cgValue yu = cg_emit_transmute(p, y, t_u64);
				TB_Node *hi = nullptr;
				TB_Node *lo = tb_builder_umulpair(cg_tb_g(p->func), xu.node, yu.node, &hi);
				cgValue lo_v = cg_value(lo, t_u64);
				cgValue hi_v = cg_value(hi, t_u64);
				res = cg_emit_transmute(p, lo_v, type);
				if (!sign) {
					ok = cg_emit_comp(p, Token_NotEq, hi_v, cg_const_int(p, t_u64, 0));
				} else {
					// signed correction: hi_s = hi_u - (x<0 ? y : 0) - (y<0 ? x : 0)
					// overflow iff hi_s != lo>>63 (arithmetic)
					cgValue zero = cg_const_int(p, t_u64, 0);
					cgValue x_neg = cg_emit_comp(p, Token_Lt, x, cg_const_int(p, type, 0));
					cgValue y_neg = cg_emit_comp(p, Token_Lt, y, cg_const_int(p, type, 0));
					cgValue corr1 = cg_emit_arith(p, Token_Mul, cg_emit_conv(p, x_neg, t_u64), yu, t_u64);
					cgValue corr2 = cg_emit_arith(p, Token_Mul, cg_emit_conv(p, y_neg, t_u64), xu, t_u64);
					cgValue hi_s = cg_emit_arith(p, Token_Sub, hi_v, corr1, t_u64);
					hi_s = cg_emit_arith(p, Token_Sub, hi_s, corr2, t_u64);
					// arithmetic shift of the raw low word (as i64) gives the
					// sign extension to compare the corrected high word against
					cgValue lo_s = cg_emit_transmute(p, lo_v, t_i64);
					cgValue sign_ext = cg_emit_arith(p, Token_Shr, lo_s, cg_const_int(p, t_i64, 63), t_i64);
					sign_ext = cg_emit_transmute(p, sign_ext, t_u64);
					ok = cg_emit_comp(p, Token_NotEq, hi_s, sign_ext);
				}
			}
			ok = cg_emit_conv(p, ok, ok_type);

			return cg_value_multi2(res, ok, res_type);
		}


	case BuiltinProc_ptr_offset:
		{
			cgValue ptr = cg_build_expr(p, ce->args[0]);
			cgValue len = cg_build_expr(p, ce->args[1]);
			len = cg_emit_conv(p, len, t_int);
			return cg_emit_ptr_offset(p, ptr, len);
		}
	case BuiltinProc_ptr_sub:
		{
			Type *elem0 = type_deref(type_of_expr(ce->args[0]));
			Type *elem1 = type_deref(type_of_expr(ce->args[1]));
			GB_ASSERT(are_types_identical(elem0, elem1));
			Type *elem = elem0;

			cgValue ptr0 = cg_emit_conv(p, cg_build_expr(p, ce->args[0]), t_uintptr);
			cgValue ptr1 = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t_uintptr);

			cgValue diff = cg_emit_arith(p, Token_Sub, ptr0, ptr1, t_uintptr);
			diff = cg_emit_conv(p, diff, t_int);
			return cg_emit_arith(p, Token_Quo, diff, cg_const_int(p, t_int, type_size_of(elem)), t_int);
		}

	case BuiltinProc_type_info_of:
		{
			Ast *arg = ce->args[0];
			TypeAndValue tav = type_and_value_of_expr(arg);
			if (tav.mode == Addressing_Type) {
				Type *t = default_type(type_of_expr(arg));
				return cg_type_info(p, t);
			}
			GB_ASSERT(is_type_typeid(tav.type));

			auto args = slice_make<cgValue>(permanent_allocator(), 1);
			args[0] = cg_build_expr(p, arg);
			return cg_emit_runtime_call(p, "__type_info_of", args);
		}


	case BuiltinProc_type_equal_proc:
		return cg_equal_proc_value_for_type(p, ce->args[0]->tav.type);

	case BuiltinProc_type_hasher_proc:
		return cg_hasher_proc_value_for_type(p, ce->args[0]->tav.type);

	case BuiltinProc_type_map_cell_info:
		return cg_builtin_map_cell_info(p, ce->args[0]->tav.type);
	case BuiltinProc_type_map_info:
		return cg_builtin_map_info(p, ce->args[0]->tav.type);

	case BuiltinProc_expect:
		{
			Type *t = default_type(expr->tav.type);
			cgValue x = cg_emit_conv(p, cg_build_expr(p, ce->args[0]), t);
			cgValue y = cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t);
			gb_unused(y);
			return x;
		}

	case BuiltinProc_count_leading_zeros:
		{
			cgValue n = cg_build_expr(p, ce->args[0]);
			n = cg_emit_conv(p, n, default_type(expr->tav.type));
			if (cg_is_type_integer_128(n.type)) {
				// NOTE(odin-tilde): hi != 0 ? clz(hi) : 64 + clz(lo)
				bool swapped = cg_u128_is_swapped(n.type);
				TB_Node *ap = cg_u128_addr_of(p, n);
				TB_Node *lo = cg_emit_u128_limb_load(p, ap, swapped, 0);
				TB_Node *hi = cg_emit_u128_limb_load(p, ap, swapped, 1);
				TB_Node *zero = tb_inst_uint(p->func, TB_TYPE_I64, 0);
				TB_Node *clz_hi = tb_inst_zxt(p->func, tb_inst_clz(p->func, hi), TB_TYPE_I64);
				TB_Node *clz_lo = tb_inst_zxt(p->func, tb_inst_clz(p->func, lo), TB_TYPE_I64);
				clz_lo = tb_inst_add(p->func, clz_lo, tb_inst_uint(p->func, TB_TYPE_I64, 64), cast(TB_ArithmeticBehavior)0);
				TB_Node *hi_nonzero = tb_inst_cmp_ne(p->func, hi, zero);
				TB_Node *cnt = tb_inst_select(p->func, hi_nonzero, clz_hi, clz_lo);
				return cg_emit_conv(p, cg_value(cnt, t_u64), n.type);
			}
			GB_ASSERT(n.kind == cgValue_Value);
			TB_Node *val = tb_inst_clz(p->func, n.node);
			val = tb_inst_zxt(p->func, val, cg_data_type(n.type));
			return cg_value(val, n.type);
		}


	case BuiltinProc_byte_swap:
		{
			cgValue n = cg_build_expr(p, ce->args[0]);
			n = cg_emit_conv(p, n, default_type(expr->tav.type));
			if (cg_is_type_integer_128(n.type)) {
				// NOTE(odin-tilde): full 16-byte reversal = swap the limbs and
				// bswap each one
				bool swapped = cg_u128_is_swapped(n.type);
				TB_Node *ap = cg_u128_addr_of(p, n);
				TB_Node *lo = cg_emit_u128_limb_load(p, ap, swapped, 0);
				TB_Node *hi = cg_emit_u128_limb_load(p, ap, swapped, 1);
				return cg_u128_make_value(p, n.type, swapped,
				                          tb_inst_bswap(p->func, hi),
				                          tb_inst_bswap(p->func, lo));
			}
			GB_ASSERT(n.kind == cgValue_Value);
			if (type_size_of(n.type) == 1) {
				return n; // bswap of a byte is the identity
			}
			TB_Node *val = tb_inst_bswap(p->func, n.node);
			return cg_value(val, n.type);
		}

	case BuiltinProc_reverse_bits:
		{
			cgValue n = cg_build_expr(p, ce->args[0]);
			n = cg_emit_conv(p, n, default_type(expr->tav.type));
			if (cg_is_type_integer_128(n.type)) {
				// NOTE(odin-tilde): reverse each limb and swap their positions
				bool swapped = cg_u128_is_swapped(n.type);
				TB_Node *ap = cg_u128_addr_of(p, n);
				TB_Node *lo = cg_emit_u128_limb_load(p, ap, swapped, 0);
				TB_Node *hi = cg_emit_u128_limb_load(p, ap, swapped, 1);
				return cg_u128_make_value(p, n.type, swapped,
				                          tb_inst_reverse_bits(p->func, hi),
				                          tb_inst_reverse_bits(p->func, lo));
			}
			GB_ASSERT(n.kind == cgValue_Value);
			TB_Node *val = tb_inst_reverse_bits(p->func, n.node);
			return cg_value(val, n.type);
		}

	case BuiltinProc_count_trailing_zeros:
		{
			cgValue n = cg_build_expr(p, ce->args[0]);
			n = cg_emit_conv(p, n, default_type(expr->tav.type));
			if (cg_is_type_integer_128(n.type)) {
				// NOTE(odin-tilde): lo != 0 ? ctz(lo) : 64 + ctz(hi)
				bool swapped = cg_u128_is_swapped(n.type);
				TB_Node *ap = cg_u128_addr_of(p, n);
				TB_Node *lo = cg_emit_u128_limb_load(p, ap, swapped, 0);
				TB_Node *hi = cg_emit_u128_limb_load(p, ap, swapped, 1);
				TB_Node *zero = tb_inst_uint(p->func, TB_TYPE_I64, 0);
				TB_Node *ctz_lo = tb_inst_zxt(p->func, tb_inst_ctz(p->func, lo), TB_TYPE_I64);
				TB_Node *ctz_hi = tb_inst_zxt(p->func, tb_inst_ctz(p->func, hi), TB_TYPE_I64);
				ctz_hi = tb_inst_add(p->func, ctz_hi, tb_inst_uint(p->func, TB_TYPE_I64, 64), cast(TB_ArithmeticBehavior)0);
				TB_Node *lo_nonzero = tb_inst_cmp_ne(p->func, lo, zero);
				TB_Node *cnt = tb_inst_select(p->func, lo_nonzero, ctz_lo, ctz_hi);
				return cg_emit_conv(p, cg_value(cnt, t_u64), n.type);
			}
			GB_ASSERT(n.kind == cgValue_Value);
			TB_Node *val = tb_inst_ctz(p->func, n.node);
			val = tb_inst_zxt(p->func, val, cg_data_type(n.type));
			return cg_value(val, n.type);
		}

	case BuiltinProc_count_ones:
		{
			cgValue n = cg_build_expr(p, ce->args[0]);
			n = cg_emit_conv(p, n, default_type(expr->tav.type));
			if (cg_is_type_integer_128(n.type)) {
				// NOTE(odin-tilde): popcount(lo) + popcount(hi)
				bool swapped = cg_u128_is_swapped(n.type);
				TB_Node *ap = cg_u128_addr_of(p, n);
				TB_Node *lo = cg_emit_u128_limb_load(p, ap, swapped, 0);
				TB_Node *hi = cg_emit_u128_limb_load(p, ap, swapped, 1);
				TB_Node *pc_lo = tb_inst_zxt(p->func, tb_inst_popcount(p->func, lo), TB_TYPE_I64);
				TB_Node *pc_hi = tb_inst_zxt(p->func, tb_inst_popcount(p->func, hi), TB_TYPE_I64);
				TB_Node *cnt = tb_inst_add(p->func, pc_lo, pc_hi, cast(TB_ArithmeticBehavior)0);
				return cg_emit_conv(p, cg_value(cnt, t_u64), n.type);
			}
			GB_ASSERT(n.kind == cgValue_Value);
			TB_Node *val = tb_inst_popcount(p->func, n.node);
			val = tb_inst_zxt(p->func, val, cg_data_type(n.type));
			return cg_value(val, n.type);
		}

	case BuiltinProc_count_zeros:
		{
			cgValue n = cg_build_expr(p, ce->args[0]);
			n = cg_emit_conv(p, n, default_type(expr->tav.type));
			if (cg_is_type_integer_128(n.type)) {
				// NOTE(odin-tilde): 128 - popcount(lo) - popcount(hi)
				bool swapped = cg_u128_is_swapped(n.type);
				TB_Node *ap = cg_u128_addr_of(p, n);
				TB_Node *lo = cg_emit_u128_limb_load(p, ap, swapped, 0);
				TB_Node *hi = cg_emit_u128_limb_load(p, ap, swapped, 1);
				TB_Node *pc_lo = tb_inst_zxt(p->func, tb_inst_popcount(p->func, lo), TB_TYPE_I64);
				TB_Node *pc_hi = tb_inst_zxt(p->func, tb_inst_popcount(p->func, hi), TB_TYPE_I64);
				TB_Node *ones = tb_inst_add(p->func, pc_lo, pc_hi, cast(TB_ArithmeticBehavior)0);
				TB_Node *cnt = tb_inst_sub(p->func, tb_inst_uint(p->func, TB_TYPE_I64, 128), ones, cast(TB_ArithmeticBehavior)0);
				return cg_emit_conv(p, cg_value(cnt, t_u64), n.type);
			}
			GB_ASSERT(n.kind == cgValue_Value);
			TB_DataType dt = cg_data_type(n.type);
			TB_Node *ones = tb_inst_popcount(p->func, n.node);
			ones = tb_inst_zxt(p->func, ones, dt);

			cgValue size = cg_const_int(p, n.type, 8*type_size_of(n.type));
			return cg_emit_arith(p, Token_Sub, size, cg_value(ones, n.type), n.type);
		}

	// PATCH(odin-tilde): TB has no inline-asm/CPUID node, and the emitted
	// x86_64 binaries typically run under Rosetta anyway. Reporting "no
	// features" keeps every core-library user on its portable fallback
	// path, which is always correct.
	case BuiltinProc_x86_cpuid:
		{
			Type *res_type = type_of_expr(expr);
			GB_ASSERT(res_type->kind == Type_Tuple);
			GB_ASSERT(res_type->Tuple.variables.count == 4);
			auto values = array_make<cgValue>(permanent_allocator(), 4);
			for (isize i = 0; i < 4; i++) {
				values[i] = cg_const_int(p, t_u32, 0);
			}
			Slice<cgValue> sv = {};
			sv.data  = values.data;
			sv.count = values.count;
			return cg_value_multi(sv, res_type);
		}

	case BuiltinProc_x86_xgetbv:
		{
			Type *res_type = type_of_expr(expr);
			GB_ASSERT(res_type->kind == Type_Tuple);
			GB_ASSERT(res_type->Tuple.variables.count == 2);
			return cg_value_multi2(cg_const_int(p, t_u32, 0), cg_const_int(p, t_u32, 0), res_type);
		}

	// PATCH(odin-tilde): without inline asm the valgrind client-request
	// sequence cannot be emitted; when not running under valgrind the real
	// thing returns its `default` argument, so just do that.
	case BuiltinProc_valgrind_client_request:
		{
			cgValue def = cg_build_expr(p, ce->args[0]);
			return cg_emit_conv(p, def, type_of_expr(expr));
		}

	case BuiltinProc_unreachable:
		tb_inst_unreachable(p->func);
		return {};

	case BuiltinProc_alloca:
		{
			// NOTE(odin-tilde): TB has no dynamic alloca (Cuik defines
			// __STDC_NO_VLA__), so this is emulated two ways:
			//   * constant size  -> a plain TB local (per-frame, so an alloca
			//     inside a loop reuses the slot each iteration instead of
			//     growing the stack; fine for all core/* uses)
			//   * dynamic size   -> runtime heap_alloc (macOS malloc gives
			//     16-byte alignment, covering every current use) and is
			//     NEVER FREED: the memory outlives the proc, which is safe
			//     for readers but leaks per call. Documented in
			//     KNOWN_ISSUES.md; a free-at-proc-exit scheme is the
			//     intended proper fix.
			i64 al = exact_value_to_i64(type_and_value_of_expr(ce->args[1]).value);
			TypeAndValue sz_tav = type_and_value_of_expr(ce->args[0]);

			TB_Node *ptr = nullptr;
			if (sz_tav.mode == Addressing_Constant) {
				i64 size = exact_value_to_i64(sz_tav.value);
				ptr = tb_inst_local(p->func, cast(TB_CharUnits)gb_max(size, 1), cast(TB_CharUnits)al);
			} else {
				cgValue sz = cg_build_expr(p, ce->args[0]);
				sz = cg_emit_conv(p, sz, t_int);
				auto args = slice_make<cgValue>(permanent_allocator(), 2);
				args[0] = sz;
				args[1] = cg_const_bool(p, t_bool, false); // zero_memory = false
				cgValue res = cg_emit_runtime_call(p, "heap_alloc", args);
				ptr = cg_flatten_value(p, res).node;
			}
			cgValue mp = cg_value(ptr, alloc_type_pointer(t_u8));
			return cg_emit_conv(p, mp, type_of_expr(expr));
		}

	case BuiltinProc_likely:
	case BuiltinProc_unlikely:
		{
			// NOTE(odin-tilde): branch-hint builtins; TB has no expect
			// intrinsic wired up here, so just pass the value through
			// (correct, only loses the hint).
			Type *t = default_type(type_of_expr(expr));
			cgValue x = cg_build_expr(p, ce->args[0]);
			return cg_emit_conv(p, x, t);
		}

	case BuiltinProc_read_cycle_counter:
		{
			// NOTE(odin-tilde): TB_CYCLE_COUNTER node; x64 lowers to rdtsc,
			// aarch64 to `mrs xN, cntvct_el0` (same register the LLVM
			// backend reads via inline asm).
			GB_ASSERT(cg_tb_builder != nullptr);
			TB_Node *n = tb_builder_cycle_counter(cg_tb_builder);
			return cg_value(n, type_of_expr(expr));
		}

	case BuiltinProc_read_cycle_counter_frequency:
		{
			// NOTE(odin-tilde): only meaningful on arm64 (cntfrq_el0); TB has
			// no node for it. Return 0 as an unknown frequency rather than
			// panicking — callers use it for unit conversion only.
			return cg_const_int(p, type_of_expr(expr), 0);
		}

	}

	GB_PANIC("TODO(bill): builtin procs %d %.*s", id, LIT(builtin_name));
	return {};
}

