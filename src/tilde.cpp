#include "tilde.hpp"


gb_global Slice<TB_Arena> global_tb_arenas;

gb_internal TB_Arena *cg_arena(void) {
	return &global_tb_arenas[current_thread_index()];
}

gb_internal void cg_global_arena_init(void) {
	global_tb_arenas = slice_make<TB_Arena>(permanent_allocator(), global_thread_pool.threads.count);
	for_array(i, global_tb_arenas) {
		tb_arena_create(&global_tb_arenas[i], "odin-tilde");
	}
}

// returns TB_TYPE_VOID if not trivially possible
gb_internal TB_DataType cg_data_type(Type *t) {
	GB_ASSERT(t != nullptr);
	t = core_type(t);
	i64 sz = type_size_of(t);
	switch (t->kind) {
	case Type_Basic:
		switch (t->Basic.kind) {
		case Basic_bool:
		case Basic_b8:
		case Basic_b16:
		case Basic_b32:
		case Basic_b64:

		case Basic_i8:
		case Basic_u8:
		case Basic_i16:
		case Basic_u16:
		case Basic_i32:
		case Basic_u32:
		case Basic_i64:
		case Basic_u64:

		// NOTE(odin-tilde): TB has no 128-bit integer type. u128/i128 (and
		// their endian variants) intentionally fall through to TB_TYPE_VOID
		// so they are treated as opaque 16-byte aggregates; every arithmetic,
		// comparison and conversion op on them is lowered limb-wise in
		// tilde_expr.cpp (see cg_emit_arith_u128).

		case Basic_rune:

		case Basic_int:
		case Basic_uint:
		case Basic_uintptr:
		case Basic_typeid:
			return TB_TYPE_INTN(cast(u16)gb_min(8*sz, 64));

		case Basic_f16: return TB_TYPE_F16;
		case Basic_f32: return TB_TYPE_F32;
		case Basic_f64: return TB_TYPE_F64;

		case Basic_rawptr:    return TB_TYPE_PTR;
		case Basic_cstring:   return TB_TYPE_PTR;
		case Basic_cstring16: return TB_TYPE_PTR;


		// Endian Specific Types
		case Basic_i16le:
		case Basic_u16le:
		case Basic_i32le:
		case Basic_u32le:
		case Basic_i64le:
		case Basic_u64le:
		case Basic_i16be:
		case Basic_u16be:
		case Basic_i32be:
		case Basic_u32be:
		case Basic_i64be:
		case Basic_u64be:
			return TB_TYPE_INTN(cast(u16)gb_min(8*sz, 64));

		case Basic_f16le: return TB_TYPE_F16;
		case Basic_f32le: return TB_TYPE_F32;
		case Basic_f64le: return TB_TYPE_F64;

		case Basic_f16be: return TB_TYPE_F16;
		case Basic_f32be: return TB_TYPE_F32;
		case Basic_f64be: return TB_TYPE_F64;
		}
		break;

	case Type_Pointer:
	case Type_MultiPointer:
	case Type_Proc:
		return TB_TYPE_PTR;

	case Type_BitSet:
		return cg_data_type(bit_set_to_int(t));

	case Type_Enum:
		// PATCH(odin-tilde): enums are represented as their underlying integer
		return cg_data_type(t->Enum.base_type);

	}

	// unknown
	return {};
}


gb_internal cgValue cg_value(TB_Global *g, Type *type) {
	return cg_value((TB_Symbol *)g, type);
}
gb_internal cgValue cg_value(TB_External *e, Type *type) {
	return cg_value((TB_Symbol *)e, type);
}
gb_internal cgValue cg_value(TB_Function *f, Type *type) {
	return cg_value((TB_Symbol *)f, type);
}
gb_internal cgValue cg_value(TB_Symbol *s, Type *type) {
	cgValue v = {};
	v.kind = cgValue_Symbol;
	v.type = type;
	v.symbol = s;
	return v;
}
gb_internal cgValue cg_value(TB_Node *node, Type *type) {
	cgValue v = {};
	v.kind = cgValue_Value;
	v.type = type;
	v.node = node;
	return v;
}
gb_internal cgValue cg_lvalue_addr(TB_Node *node, Type *type) {
	GB_ASSERT(node->dt.type == TB_TAG_PTR);
	cgValue v = {};
	v.kind = cgValue_Addr;
	v.type = type;
	v.node = node;
	return v;
}

gb_internal cgValue cg_lvalue_addr_to_value(cgValue v) {
	if (v.kind == cgValue_Value) {
		GB_ASSERT(is_type_pointer(v.type));
		GB_ASSERT(v.node->dt.type == TB_TAG_PTR);
	} else {
		GB_ASSERT(v.kind == cgValue_Addr);
		GB_ASSERT(v.node->dt.type == TB_TAG_PTR);
		v.kind = cgValue_Value;
		v.type = alloc_type_pointer(v.type);
	}
	return v;
}

gb_internal cgValue cg_value_multi(cgValueMulti *multi, Type *type) {
	GB_ASSERT(type->kind == Type_Tuple);
	GB_ASSERT(multi != nullptr);
	GB_ASSERT(type->Tuple.variables.count > 1);
	GB_ASSERT(multi->values.count == type->Tuple.variables.count);
	cgValue v = {};
	v.kind = cgValue_Multi;
	v.type = type;
	v.multi = multi;
	return v;
}

gb_internal cgValue cg_value_multi(Slice<cgValue> const &values, Type *type) {
	cgValueMulti *multi = gb_alloc_item(permanent_allocator(), cgValueMulti);
	multi->values = values;
	return cg_value_multi(multi, type);
}


gb_internal cgValue cg_value_multi2(cgValue const &x, cgValue const &y, Type *type) {
	GB_ASSERT(type->kind == Type_Tuple);
	GB_ASSERT(type->Tuple.variables.count == 2);
	cgValueMulti *multi = gb_alloc_item(permanent_allocator(), cgValueMulti);
	multi->values = slice_make<cgValue>(permanent_allocator(), 2);
	multi->values[0] = x;
	multi->values[1] = y;
	return cg_value_multi(multi, type);
}


gb_internal cgAddr cg_addr(cgValue const &value) {
	GB_ASSERT(value.kind != cgValue_Multi);
	cgAddr addr = {};
	addr.kind = cgAddr_Default;
	addr.addr = value;
	if (addr.addr.kind == cgValue_Addr) {
		GB_ASSERT(addr.addr.node != nullptr);
		addr.addr.kind = cgValue_Value;
		addr.addr.type = alloc_type_pointer(addr.addr.type);
	}
	return addr;
}

gb_internal cgAddr cg_addr_map(cgValue addr, cgValue map_key, Type *map_type, Type *map_result) {
	GB_ASSERT(is_type_pointer(addr.type));
	Type *mt = type_deref(addr.type);
	GB_ASSERT(is_type_map(mt));

	cgAddr v = {cgAddr_Map, addr};
	v.map.key    = map_key;
	v.map.type   = map_type;
	v.map.result = map_result;
	return v;
}

gb_internal cgAddr cg_addr_soa_variable(cgValue addr, cgValue index, Ast *index_expr) {
	cgAddr v = {cgAddr_SoaVariable, addr};
	v.soa.index = index;
	v.soa.index_expr = index_expr;
	return v;
}

gb_internal cgAddr cg_addr_bit_field(cgValue addr, Type *type, i64 bit_offset, u8 bit_size) {
	GB_ASSERT(is_type_pointer(addr.type));
	Type *mt = type_deref(addr.type);
	GB_ASSERT_MSG(is_type_bit_field(mt), "%s", type_to_string(mt));

	cgAddr v = {cgAddr_BitField, addr};
	v.bitfield.type       = type;
	v.bitfield.bit_offset = bit_offset;
	v.bitfield.bit_size   = bit_size;
	return v;
}

gb_internal cgAddr cg_addr_swizzle(cgValue addr, Type *type, u8 swizzle_count, u8 swizzle_indices[4]) {
	GB_ASSERT(is_type_array(type) || is_type_simd_vector(type));
	GB_ASSERT(1 < swizzle_count && swizzle_count <= 4);
	cgAddr v = {cgAddr_Swizzle, addr};
	v.swizzle.type = type;
	v.swizzle.count = swizzle_count;
	gb_memmove(v.swizzle.indices, swizzle_indices, swizzle_count);
	return v;
}



gb_internal void cg_set_debug_pos_from_node(cgProcedure *p, Ast *node) {
	// NOTE(odin-tilde): debug info export is not wired up (we always export
	// TB_DEBUGFMT_NONE), so TB_DEBUG_LOCATION nodes are pure overhead — and
	// worse, a location node landing as a block end crashes Cuik's late CFG
	// computation (cfg.h tb_todo, seen compiling net@_set_option). Skip
	// emission entirely until debug export exists.
#if 0
	if (node) {
		TokenPos pos = ast_token(node).pos;
		// PATCH(odin-tilde): synthesized AST nodes (e.g. inside generated
		// equal/hasher procs) can carry file_id 0, which is not a valid
		// key in file_id_map (ptr_map panics on a 0 key).
		if (pos.file_id == 0) {
			return;
		}
		TB_SourceFile **file = map_get(&p->module->file_id_map, cast(uintptr)pos.file_id);
		if (file) {
			tb_inst_location(p->func, *file, pos.line, pos.column);
		}
	}
#endif
}

gb_internal void cg_add_symbol(cgModule *m, Entity *e, TB_Symbol *symbol) {
	if (e) {
		rw_mutex_lock(&m->values_mutex);
		map_set(&m->symbols, e, symbol);
		rw_mutex_unlock(&m->values_mutex);
	}
}

gb_internal void cg_add_entity(cgModule *m, Entity *e, cgValue const &val) {
	if (e) {
		rw_mutex_lock(&m->values_mutex);
		GB_ASSERT(val.node != nullptr);
		map_set(&m->values, e, val);
		rw_mutex_unlock(&m->values_mutex);
	}
}

gb_internal void cg_add_member(cgModule *m, String const &name, cgValue const &val) {
	if (name.len > 0) {
		rw_mutex_lock(&m->values_mutex);
		string_map_set(&m->members, name, val);
		rw_mutex_unlock(&m->values_mutex);
	}
}

gb_internal void cg_add_procedure_value(cgModule *m, cgProcedure *p) {
	rw_mutex_lock(&m->values_mutex);
	if (p->entity != nullptr) {
		if (p->func != nullptr) { // NOTE(new-tb): foreign/extern procs have no TB_Function
			map_set(&m->procedure_values, p->func, p->entity);
		}
		if (p->symbol != nullptr) {
			map_set(&m->symbols, p->entity, p->symbol);
		}
	}
	string_map_set(&m->procedures, p->name, p);
	rw_mutex_unlock(&m->values_mutex);

}

gb_internal TB_Symbol *cg_find_symbol_from_entity(cgModule *m, Entity *e) {
	GB_ASSERT(e != nullptr);

	rw_mutex_lock(&m->values_mutex);
	TB_Symbol **found = map_get(&m->symbols, e);
	if (found) {
		rw_mutex_unlock(&m->values_mutex);
		return *found;
	}

	String link_name = cg_get_entity_name(m, e);
	cgProcedure **proc_found = string_map_get(&m->procedures, link_name);
	if (proc_found) {
		TB_Symbol *symbol = (*proc_found)->symbol;
		map_set(&m->symbols, e, symbol);
		rw_mutex_unlock(&m->values_mutex);
		return symbol;
	}
	rw_mutex_unlock(&m->values_mutex);

	if (e->kind == Entity_Procedure) {
		debugf("[Tilde] try to generate procedure %.*s as it was not in the minimum_dependency_set", LIT(e->token.string));
		// IMPORTANT TODO(bill): This is an utter bodge, try and fix this shit
		cgProcedure *p = cg_procedure_create(m, e);
		if (p != nullptr) {
			GB_ASSERT(p->symbol != nullptr);
			cg_add_procedure_to_queue(p);
			return p->symbol;
		}
	}


	GB_PANIC("could not find entity's symbol %.*s", LIT(e->token.string));
	return nullptr;
}


struct cgGlobalVariable {
	cgValue var;
	cgValue init;
	DeclInfo *decl;
	bool is_initialized;
};

// Returns already_has_entry_point
gb_internal bool cg_global_variables_create(cgModule *m, Array<cgGlobalVariable> *global_variables) {
	isize global_variable_max_count = 0;
	bool already_has_entry_point = false;

	for (Entity *e : m->info->entities) {
		String name = e->token.string;

		if (e->kind == Entity_Variable) {
			global_variable_max_count++;
		} else if (e->kind == Entity_Procedure) {
			if ((e->scope->flags&ScopeFlag_Init) && name == "main") {
				GB_ASSERT(e == m->info->entry_point);
			}
			if (build_context.command_kind == Command_test &&
			    (e->Procedure.is_export || e->Procedure.link_name.len > 0)) {
				String link_name = e->Procedure.link_name;
				if (e->pkg->kind == Package_Runtime) {
					if (link_name == "main"           ||
					    link_name == "DllMain"        ||
					    link_name == "WinMain"        ||
					    link_name == "wWinMain"       ||
					    link_name == "mainCRTStartup" ||
					    link_name == "_start") {
						already_has_entry_point = true;
					}
				}
			}
		}
	}
	*global_variables = array_make<cgGlobalVariable>(permanent_allocator(), 0, global_variable_max_count);


	for (DeclInfo *d : m->info->variable_init_order) {
		Entity *e = d->entity;

		if ((e->scope->flags & ScopeFlag_File) == 0) {
			continue;
		}

		if (!(e->min_dep_count.load(std::memory_order_relaxed) > 0)) {
			continue;
		}

		DeclInfo *decl = decl_info_of_entity(e);
		if (decl == nullptr) {
			continue;
		}
		GB_ASSERT(e->kind == Entity_Variable);

		bool is_foreign = e->Variable.is_foreign;
		bool is_export  = e->Variable.is_export;

		String name = cg_get_entity_name(m, e);

		TB_Linkage linkage = TB_LINKAGE_PRIVATE;

		if (is_foreign) {
			linkage = TB_LINKAGE_PUBLIC;
			cg_add_foreign_library_path(m, e->Variable.foreign_library);
		} else if (is_export) {
			linkage = TB_LINKAGE_PUBLIC;
		}
		// lb_set_linkage_from_entity_flags(m, g.value, e->flags);

		TB_DebugType *debug_type = cg_debug_type(m, e->type);
		TB_Global *global = tb_global_create(m->mod, name.len, cast(char const *)name.text, debug_type, linkage);
		cgValue g = cg_value(global, alloc_type_pointer(e->type));

		TB_ModuleSectionHandle section = tb_module_get_data(m->mod);

		if (e->Variable.thread_local_model != "") {
			section = tb_module_get_tls(m->mod);
			// NOTE(odin-tilde): TLS shim tables are calloc-zeroed per thread;
			// only zero initializers are semantically supported for now.
			if (decl->init_expr != nullptr) {
				TypeAndValue tav = type_and_value_of_expr(decl->init_expr);
				if (tav.mode == Addressing_Constant && tav.value.kind != ExactValue_Invalid) {
					cg_check_thread_local_initializer(e, &tav.value);
				} else if (!is_type_untyped_nil(tav.type)) {
					cg_check_thread_local_initializer(e, nullptr);
				}
			}
		}
		if (e->Variable.link_section.len > 0) {
			// TODO(bill): custom module sections
			// LLVMSetSection(g.value, alloc_cstring(permanent_allocator(), e->Variable.link_section));
		}


		cgGlobalVariable var = {};
		var.var = g;
		var.decl = decl;

		if (decl->init_expr != nullptr) {
			TypeAndValue tav = type_and_value_of_expr(decl->init_expr);

			isize max_regions = cg_global_const_calculate_region_count(tav.value, e->type);
			// NOTE(odin-tilde): Cuik requires size > 0; a zero-sized global
			// (e.g. struct {}) gets a dummy byte of storage — nobody reads it.
			i64 gv_size  = gb_max(type_size_of(e->type),  1);
			i64 gv_align = gb_max(type_align_of(e->type), 1);
			tb_global_set_storage(m->mod, section, global, gv_size, gv_align, max_regions);

			if (tav.mode == Addressing_Constant &&
			    tav.value.kind != ExactValue_Invalid) {
				cg_global_const_add_region(m, tav.value, e->type, global, 0);
				var.is_initialized = true;
			}
			if (!var.is_initialized && is_type_untyped_nil(tav.type)) {
				var.is_initialized = true;
			}
		} else {
			var.is_initialized = true;
			// TODO(bill): is this even needed;
			i64 max_regions = cg_global_const_calculate_region_count_from_basic_type(e->type);
			i64 gv_size  = gb_max(type_size_of(e->type),  1);
			i64 gv_align = gb_max(type_align_of(e->type), 1);
			tb_global_set_storage(m->mod, section, global, gv_size, gv_align, max_regions);
		}

		array_add(global_variables, var);

		cg_add_symbol(m, e, cast(TB_Symbol *)global);
		cg_add_entity(m, e, g);
		cg_add_member(m, name, g);
	}

	cg_setup_type_info_data(m);

	return already_has_entry_point;
}

gb_internal void cg_global_variables_initialize(cgProcedure *p, Array<cgGlobalVariable> *global_variables) {
	for (cgGlobalVariable &var : *global_variables) {
		if (var.is_initialized) {
			continue;
		}
		cgValue src = cg_build_expr(p, var.decl->init_expr);
		cgValue dst = cg_flatten_value(p, var.var);
		cg_emit_store(p, dst, src);
	}
}


gb_global bool cg_tb_verbose_codegen = true; // TEMP debug

gb_internal void cg_add_foreign_library_path(cgModule *m, Entity *e) {
	if (e == nullptr) {
		return;
	}
	GB_ASSERT(e->kind == Entity_LibraryName);

	LinkerData *ld = m->linker_data;
	GB_ASSERT(ld != nullptr);

	mutex_lock(&ld->foreign_mutex);
	if (!ptr_set_update(&ld->foreign_libraries_set, e)) {
		array_add(&ld->foreign_libraries, e);
	}
	mutex_unlock(&ld->foreign_mutex);
}

// NOTE(odin-tilde): deterministic build support — see the comment on
// entity_ranks/entity_ordinals in cgModule (tilde.hpp).
gb_internal int cg_entity_deterministic_cmp(void const *a, void const *b) {
	Entity *ea = *cast(Entity * const *)a;
	Entity *eb = *cast(Entity * const *)b;

	String pa = ea->pkg ? ea->pkg->name : String{};
	String pb = eb->pkg ? eb->pkg->name : String{};
	i32 c = string_compare(pa, pb);
	if (c != 0) return c;
	c = string_compare(ea->token.string, eb->token.string);
	if (c != 0) return c;
	// Within one (pkg, name) group the source position is NOT a stable key:
	// polymorphic specializations record the token of whichever call site
	// created them first, which is thread-scheduling dependent. The type
	// string is structural and stable, so it leads the intra-group order.
	if (ea->type != eb->type) {
		gbString sa = type_to_string(ea->type);
		gbString sb = type_to_string(eb->type);
		String ta = make_string(cast(u8 const *)sa, gb_string_length(sa));
		String tb = make_string(cast(u8 const *)sb, gb_string_length(sb));
		c = string_compare(ta, tb);
		if (c != 0) return c;
	}
	if (ea->token.pos.file_id != eb->token.pos.file_id) return ea->token.pos.file_id < eb->token.pos.file_id ? -1 : 1;
	if (ea->token.pos.line   != eb->token.pos.line)   return ea->token.pos.line   < eb->token.pos.line   ? -1 : 1;
	if (ea->token.pos.column != eb->token.pos.column) return ea->token.pos.column < eb->token.pos.column ? -1 : 1;
	if (ea->kind != eb->kind) return ea->kind < eb->kind ? -1 : 1;
	// Last-resort tiebreak for entities identical in every deterministic
	// field (e.g. multiple `_` blanks). The racy id only orders entities
	// that never receive symbols, so it cannot leak into the output.
	if (ea->id != eb->id) return ea->id < eb->id ? -1 : 1;
	return 0;
}

gb_internal void cg_build_deterministic_entity_order(cgModule *m) {
	map_init(&m->entity_ranks);
	map_init(&m->entity_ordinals);
	m->entities_sorted = array_make<Entity *>(heap_allocator(), 0, m->info->entities.count);
	for (Entity *e : m->info->entities) {
		array_add(&m->entities_sorted, e);
	}
	gb_sort_array(m->entities_sorted.data, m->entities_sorted.count, cg_entity_deterministic_cmp);
	u64 ordinal = 0;
	for_array(i, m->entities_sorted) {
		Entity *e = m->entities_sorted[i];
		map_set(&m->entity_ranks, e, cast(u64)i);
		if (i > 0) {
			Entity *p = m->entities_sorted[i-1];
			if (p->pkg == e->pkg && p->token.string == e->token.string) {
				ordinal += 1;
			} else {
				ordinal = 0;
			}
		} else {
			ordinal = 0;
		}
		map_set(&m->entity_ordinals, e, ordinal);
	}
}

// NOTE(odin-tilde): one deterministic suffix source for all mangled names.
// Entities registered in info->entities use their name-group ordinal;
// entities that never enter that array (e.g. proc-local statics, which only
// live in their declaration scope) fall back to their declaration position
// (stable for such entities), formatted with 'x' so it can never collide
// numerically with an ordinal. The racy entity id is the last resort for
// positionless generated entities.
gb_internal isize cg_deterministic_suffix_into(cgModule *m, Entity *e, char *buf, isize buf_len) {
	u64 *ord = m ? map_get(&m->entity_ordinals, e) : nullptr;
	if (ord != nullptr) {
		return gb_snprintf(buf, buf_len, "%llu", cast(unsigned long long)*ord);
	}
	if (e->token.pos.file_id != 0) {
		return gb_snprintf(buf, buf_len, "%ux%u", cast(u32)e->token.pos.line, cast(u32)e->token.pos.column);
	}
	return gb_snprintf(buf, buf_len, "%llu", cast(unsigned long long)e->id);
}

// NOTE(odin-tilde): deterministic names for generated globals/procs.
// Proc-context creations are numbered with a per-proc counter and embed the
// (unique, stable) proc link name, so the result does not depend on worker
// interleaving; module-level creations fall back to a module counter and are
// only used from sequential phases (global-data and type-info emission).
gb_internal String cg_generated_name(cgModule *m, cgProcedure *p, char const *prefix) {
	isize prefix_len = gb_strlen(prefix);
	isize cap = prefix_len + 32;
	if (p != nullptr) {
		cap += p->name.len;
	}
	char *buf = gb_alloc_array(permanent_allocator(), char, cap);
	isize n;
	if (p != nullptr) {
		n = gb_snprintf(buf, cap, "%s$%.*s$%u", prefix, LIT(p->name), 1+p->generated_name_guid++);
	} else {
		n = gb_snprintf(buf, cap, "%s$m$%u", prefix, 1+m->const_nil_guid.fetch_add(1));
	}
	return make_string(cast(u8 *)buf, n-1);
}

// NOTE(odin-tilde): deterministic name for type-keyed generated symbols
// (equal/hasher procs, map info globals). The creation sites dedup through a
// PtrMap<Type*, ...>, so a hash of the type string is stable and unique in
// practice; a collision would surface as a TB symbol-collision panic.
gb_internal String cg_typed_generated_name(char const *prefix, Type *type) {
	gbString ts = type_to_string(type);
	u64 h = gb_murmur64(ts, gb_string_length(ts));
	gb_string_free(ts);
	char buf[64] = {};
	isize n = gb_snprintf(buf, 63, "%s$%llx", prefix, cast(unsigned long long)h);
	return make_string(cast(u8 *)gb_alloc_str_len(permanent_allocator(), buf, n-1), n-1);
}

// NOTE(odin-tilde): the checker sorts @(init)/@(fini) procs primarily by
// pkg->order, which depends on package load order and is not stable across
// builds; re-sort with a fully deterministic key (pkg name, file path,
// source order, offset) before emitting startup/cleanup calls.
gb_internal GB_COMPARE_PROC(cg_init_fini_cmp) {
	Entity *x = *cast(Entity **)a;
	Entity *y = *cast(Entity **)b;
	if (x == y) {
		return 0;
	}
	String px = x->pkg ? x->pkg->name : (String{});
	String py = y->pkg ? y->pkg->name : (String{});
	int cmp = string_compare(px, py);
	if (cmp) {
		return cmp;
	}
	String fx = x->file ? x->file->fullpath : (String{});
	String fy = y->file ? y->file->fullpath : (String{});
	cmp = string_compare(fx, fy);
	if (cmp) {
		return cmp;
	}
	cmp = u64_cmp(x->order_in_src, y->order_in_src);
	if (cmp) {
		return cmp;
	}
	return i32_cmp(x->token.pos.offset, y->token.pos.offset);
}

gb_internal cgModule *cg_module_create(Checker *c) {
	cgModule *m = gb_alloc_item(permanent_allocator(), cgModule);

	m->checker = c;
	m->info = &c->info;


	bool is_jit = false;
	// NOTE(macos): x86_64 + SystemV ABI for darwin_amd64 output (runs under
	// Rosetta on Apple Silicon). TLS index is a Windows concept — omitted.
	m->mod = tb_module_create(TB_ARCH_X86_64, TB_SYSTEM_MACOS, is_jit);
	m->fence_dummy = nullptr;

	map_init(&m->values);
	map_init(&m->symbols);
	map_init(&m->file_id_map);
	map_init(&m->debug_type_map);
	map_init(&m->proc_debug_type_map);
	map_init(&m->proc_proto_map);
	map_init(&m->anonymous_proc_lits_map);
	map_init(&m->equal_procs);
	map_init(&m->hasher_procs);
	map_init(&m->map_get_procs);
	map_init(&m->map_set_procs);
	map_init(&m->map_info_map);
	map_init(&m->map_cell_info_map);
	string_map_init(&m->generated_globals_by_name);
	string_map_init(&m->generated_procs_by_name);

	array_init(&m->single_threaded_procedure_queue, heap_allocator());


	for_array(id, global_files) {
		if (AstFile *f = global_files[id]) {
			char const *path = alloc_cstring(temporary_allocator(), f->fullpath);
			TB_SourceFile *file = tb_get_source_file(m->mod, -1, path);
			map_set(&m->file_id_map, cast(uintptr)id, file);
		}
	}

	return m;
}

gb_internal void cg_module_destroy(cgModule *m) {
	map_destroy(&m->values);
	map_destroy(&m->symbols);
	map_destroy(&m->file_id_map);
	map_destroy(&m->debug_type_map);
	map_destroy(&m->proc_debug_type_map);
	map_destroy(&m->proc_proto_map);
	map_destroy(&m->anonymous_proc_lits_map);
	map_destroy(&m->equal_procs);
	map_destroy(&m->hasher_procs);
	map_destroy(&m->map_get_procs);
	map_destroy(&m->map_set_procs);
	map_destroy(&m->map_info_map);
	map_destroy(&m->map_cell_info_map);
	string_map_destroy(&m->generated_globals_by_name);
	string_map_destroy(&m->generated_procs_by_name);

	array_free(&m->single_threaded_procedure_queue);

	tb_module_destroy(m->mod);
}

gb_internal String cg_set_nested_type_name_ir_mangled_name(Entity *e, cgProcedure *p) {
	// NOTE(bill, 2020-03-08): A polymorphic procedure may take a nested type declaration
	// and as a result, the declaration does not have time to determine what it should be

	GB_ASSERT(e != nullptr && e->kind == Entity_TypeName);
	if (e->TypeName.ir_mangled_name.len != 0)  {
		return e->TypeName.ir_mangled_name;
	}
	GB_ASSERT((e->scope->flags & ScopeFlag_File) == 0);

	if (p == nullptr) {
		Entity *proc = nullptr;
		DeclInfo *parent_proc_decl = e->parent_proc_decl.load(std::memory_order_relaxed);
		if (parent_proc_decl != nullptr) {
			proc = parent_proc_decl->entity;
		} else {
			Scope *scope = e->scope;
			while (scope != nullptr && (scope->flags & ScopeFlag_Proc) == 0) {
				scope = scope->parent;
			}
			GB_ASSERT(scope != nullptr);
			GB_ASSERT(scope->flags & ScopeFlag_Proc);
			proc = scope->procedure_entity;
		}
		GB_ASSERT(proc->kind == Entity_Procedure);
		if (proc->cg_procedure != nullptr) {
			p = proc->cg_procedure;
		}
	}

	// NOTE(bill): Generate a new name
	// parent_proc.name-guid
	String ts_name = e->token.string;

	if (p != nullptr) {
		isize name_len = p->name.len + 1 + ts_name.len + 1 + 10 + 1;
		char *name_text = gb_alloc_array(permanent_allocator(), char, name_len);
		u32 guid = 1+p->module->nested_type_name_guid.fetch_add(1);
		name_len = gb_snprintf(name_text, name_len, "%.*s" ABI_PKG_NAME_SEPARATOR "%.*s-%u", LIT(p->name), LIT(ts_name), guid);

		String name = make_string(cast(u8 *)name_text, name_len-1);
		e->TypeName.ir_mangled_name = name;
		return name;
	} else {
		// NOTE(bill): a nested type be required before its parameter procedure exists. Just give it a temp name for now
		isize name_len = 9 + 1 + ts_name.len + 1 + 10 + 1;
		char *name_text = gb_alloc_array(permanent_allocator(), char, name_len);
		static std::atomic<u32> guid;
		name_len = gb_snprintf(name_text, name_len, "_internal" ABI_PKG_NAME_SEPARATOR "%.*s-%u", LIT(ts_name), 1+guid.fetch_add(1));

		String name = make_string(cast(u8 *)name_text, name_len-1);
		e->TypeName.ir_mangled_name = name;
		return name;
	}
}

gb_internal String cg_mangle_name(cgModule *m, Entity *e) {
	String name = e->token.string;

	AstPackage *pkg = e->pkg;
	GB_ASSERT_MSG(pkg != nullptr, "Missing package for '%.*s'", LIT(name));
	String pkgn = pkg->name;
	GB_ASSERT(!rune_is_digit(pkgn[0]));
	if (pkgn == "llvm") {
		GB_PANIC("llvm. entities are not allowed with the tilde backend");
	}

	isize max_len = pkgn.len + 1 + name.len + 1;
	bool require_suffix_id = is_type_polymorphic(e->type, true);

	if ((e->scope->flags & (ScopeFlag_File | ScopeFlag_Pkg)) == 0) {
		require_suffix_id = true;
	} else if (is_blank_ident(e->token)) {
		require_suffix_id = true;
	}if (e->flags & EntityFlag_NotExported) {
		require_suffix_id = true;
	}

	if (require_suffix_id) {
		max_len += 24;
	}

	char *new_name = gb_alloc_array(permanent_allocator(), char, max_len);
	isize new_name_len = gb_snprintf(
		new_name, max_len,
		"%.*s" ABI_PKG_NAME_SEPARATOR "%.*s", LIT(pkgn), LIT(name)
	);
	if (require_suffix_id) {
		char *str = new_name + new_name_len-1;
		isize len = max_len-new_name_len;
		// NOTE(odin-tilde): deterministic suffix (name-group ordinal /
		// declaration position) instead of the racy checker-assigned id.
		isize extra = 1 + cg_deterministic_suffix_into(m, e, str+1, len-1);
		str[0] = '-';
		new_name_len += extra-1;
	}

	String mangled_name = make_string((u8 const *)new_name, new_name_len-1);
	return mangled_name;
}

gb_internal String cg_get_entity_name(cgModule *m, Entity *e) {
	if (e != nullptr && e->kind == Entity_TypeName && e->TypeName.ir_mangled_name.len != 0) {
		return e->TypeName.ir_mangled_name;
	}
	GB_ASSERT(e != nullptr);

	if (e->pkg == nullptr) {
		return e->token.string;
	}

	if (e->kind == Entity_TypeName && (e->scope->flags & ScopeFlag_File) == 0) {
		return cg_set_nested_type_name_ir_mangled_name(e, nullptr);
	}

	String name = {};

	bool no_name_mangle = false;

	if (e->kind == Entity_Variable) {
		bool is_foreign = e->Variable.is_foreign;
		bool is_export  = e->Variable.is_export;
		no_name_mangle = e->Variable.link_name.len > 0 || is_foreign || is_export;
		if (e->Variable.link_name.len > 0) {
			return e->Variable.link_name;
		}
	} else if (e->kind == Entity_Procedure && e->Procedure.link_name.len > 0) {
		String link_name = e->Procedure.link_name;
		// NOTE(tilde): core:math declares its basic float ops as foreign procs
		// with LLVM intrinsic link names (llvm.sin.f32 & co). The LLVM backend
		// resolves those to real intrinsics; tilde has no such namespace, so
		// remap them to the plain libm symbols (identical signatures, and the
		// x64 SysV float ABI makes "none"/"c" calling conventions agree here).
		if (string_starts_with(link_name, str_lit("llvm."))) {
			struct cgLLVMMathRemap { String llvm, libm; };
			static const cgLLVMMathRemap llvm_math_remap[] = {
				{str_lit("llvm.sin.f32"),      str_lit("sinf")},
				{str_lit("llvm.sin.f64"),      str_lit("sin")},
				{str_lit("llvm.cos.f32"),      str_lit("cosf")},
				{str_lit("llvm.cos.f64"),      str_lit("cos")},
				{str_lit("llvm.pow.f32"),      str_lit("powf")},
				{str_lit("llvm.pow.f64"),      str_lit("pow")},
				{str_lit("llvm.exp.f32"),      str_lit("expf")},
				{str_lit("llvm.exp.f64"),      str_lit("exp")},
				{str_lit("llvm.fmuladd.f32"),  str_lit("fmaf")},
				{str_lit("llvm.fmuladd.f64"),  str_lit("fma")},
				// f16 variants have no libm equivalent; left unmapped on purpose.
			};
			for (const cgLLVMMathRemap &r : llvm_math_remap) {
				if (link_name == r.llvm) {
					return r.libm;
				}
			}
		}
		return link_name;
	} else if (e->kind == Entity_Procedure && e->Procedure.is_export) {
		no_name_mangle = true;
	}

	if (!no_name_mangle) {
		name = cg_mangle_name(m, e);
	}
	if (name.len == 0) {
		name = e->token.string;
	}

	if (e->kind == Entity_TypeName) {
		e->TypeName.ir_mangled_name = name;
	} else if (e->kind == Entity_Procedure) {
		e->Procedure.link_name = name;
	}

	return name;
}

#include "tilde_const.cpp"
#include "tilde_abi.cpp"
#include "tilde_debug.cpp"
#include "tilde_expr.cpp"
#include "tilde_builtin.cpp"
#include "tilde_type_info.cpp"
#include "tilde_proc.cpp"
#include "tilde_stmt.cpp"


gb_internal String cg_filepath_obj_for_module(cgModule *m, bool use_assembly) {
	String path = concatenate3_strings(permanent_allocator(),
		build_context.build_paths[BuildPath_Output].basename,
		STR_LIT("/"),
		build_context.build_paths[BuildPath_Output].name
	);

	// if (m->file) {
	// 	char buf[32] = {};
	// 	isize n = gb_snprintf(buf, gb_size_of(buf), "-%u", m->file->id);
	// 	String suffix = make_string((u8 *)buf, n-1);
	// 	path = concatenate_strings(permanent_allocator(), path, suffix);
	// } else if (m->pkg) {
	// 	path = concatenate3_strings(permanent_allocator(), path, STR_LIT("-"), m->pkg->name);
	// }

	String ext = {};

	if (use_assembly) {
		ext = STR_LIT(".S");
	} else {
		if (is_arch_wasm()) {
			ext = STR_LIT(".wasm.o");
		} else {
			switch (build_context.metrics.os) {
			case TargetOs_windows:
				ext = STR_LIT(".obj");
				break;
			default:
			case TargetOs_darwin:
			case TargetOs_linux:
				ext = STR_LIT(".o");
				break;

			case TargetOs_freestanding:
				switch (build_context.metrics.abi) {
				default:
				case TargetABI_Default:
				case TargetABI_SysV:
					ext = STR_LIT(".o");
					break;
				case TargetABI_Win64:
					ext = STR_LIT(".obj");
					break;
				}
				break;
			}
		}
	}

	return concatenate_strings(permanent_allocator(), path, ext);
}


gb_internal WORKER_TASK_PROC(cg_procedure_generate_worker_proc) {
	cgProcedure *p = cast(cgProcedure *)data;
	cg_procedure_generate(p);
	return 0;
}

gb_internal void cg_add_procedure_to_queue(cgProcedure *p) {
	if (p == nullptr) {
		return;
	}
	cgModule *m = p->module;
	if (m->do_threading) {
		thread_pool_add_task(cg_procedure_generate_worker_proc, p);
	} else {
		array_add(&m->single_threaded_procedure_queue, p);
	}
}

gb_internal cgValue cg_find_runtime_value(cgModule *m, String const &name) {
	AstPackage *pkg = m->info->runtime_package;
	Entity *e = scope_lookup_current(pkg->scope, string_interner_insert(name));
	return cg_find_value_from_entity(m, e);
}
gb_internal cgValue cg_find_package_value(cgModule *m, String const &pkg, String const &name) {
	Entity *e = find_entity_in_pkg(m->info, pkg, name);
	return cg_find_value_from_entity(m, e);
}

// NOTE(odin-tilde): port of lb_create_main_procedure — synthesizes the C
// entry point for `odin test` (the runtime's own main is compiled out under
// ODIN_TEST). Only the unix/CRT path is implemented: the tilde backend
// currently targets macOS (x86_64) only. Windows DllMain/mainCRTStartup and
// wasm _start variants from lb_create_main_procedure are intentionally left
// out until those targets exist.
gb_internal cgProcedure *cg_create_main_procedure(cgModule *m, cgProcedure *startup_runtime, cgProcedure *cleanup_runtime) {
	Type *params  = alloc_type_tuple();
	Type *results = alloc_type_tuple();

	Type *t_ptr_cstring = alloc_type_pointer(t_cstring);

	String name = str_lit("main");
	bool has_args = true;
	slice_init(&params->Tuple.variables, permanent_allocator(), 2);
	params->Tuple.variables[0] = alloc_entity_param(nullptr, make_token_ident("argc"), t_i32,         false, true);
	params->Tuple.variables[1] = alloc_entity_param(nullptr, make_token_ident("argv"), t_ptr_cstring, false, true);

	slice_init(&results->Tuple.variables, permanent_allocator(), 1);
	results->Tuple.variables[0] = alloc_entity_param(nullptr, blank_token, t_i32, false, true);

	Type *proc_type = alloc_type_proc(nullptr,
		params, params->Tuple.variables.count,
		results, results->Tuple.variables.count, false, ProcCC_CDecl);

	// main must be visible to libc — create it PUBLIC from the start (mutating
	// linkage after tb_function_create corrupts TB's symbol hash set).
	cgProcedure *p = cg_procedure_create_dummy(m, name, proc_type, TB_LINKAGE_PUBLIC);
	p->is_startup = true;

	// dummy proc has no Ast body; tb_builder_enter applies the prototype
	// itself, so skip cg_procedure_begin entirely (same idiom as the
	// startup/cleanup runtime procs in cg_generate_code).
	TB_GraphBuilder *g = tb_builder_enter(p->func, tb_module_get_text(m->mod), p->proto, nullptr);
	cg_tb_push_builder(p->func, g);

	// C-decl procs get a fresh (zeroed) local context; initialize it like
	// lb_emit_init_context does so the test runner has usable allocators.
	cgAddr ctx_addr = cg_find_or_generate_context_ptr(p);
	{
		Entity *e = scope_lookup_current(m->info->runtime_package->scope, string_interner_insert(str_lit("__init_context")));
		GB_ASSERT_MSG(e != nullptr, "missing runtime.__init_context");
		cgValue init_context = cg_find_procedure_value_from_entity(m, e);
		TEMPORARY_ALLOCATOR_GUARD();
		auto init_args = slice_make<cgValue>(temporary_allocator(), 1);
		init_args[0] = ctx_addr.addr;
		cg_emit_call(p, init_context, init_args);
	}

	if (has_args) { // initialize `runtime.args__`
		cgValue argc = cg_value(tb_inst_param(p->func, 0), t_i32);
		cgValue argv = cg_value(tb_inst_param(p->func, 1), t_ptr_cstring);
		argc = cg_emit_conv(p, argc, t_int);
		cgAddr args = cg_addr(cg_find_runtime_value(m, str_lit("args__")));
		cg_fill_slice(p, args, argv, argc);
	}

	cgValue startup_runtime_value = cg_value(startup_runtime->symbol, startup_runtime->type);
	cg_emit_call(p, startup_runtime_value, {});

	GB_ASSERT(build_context.command_kind == Command_test);
	{
		Type *t_Internal_Test = find_type_in_pkg(m->info, str_lit("testing"), str_lit("Internal_Test"));
		Type *array_type = alloc_type_array(t_Internal_Test, m->info->testing_procedures.count);
		Type *slice_type = alloc_type_slice(t_Internal_Test);

		String arr_name = str_lit("__$all_tests_array");
		TB_Global *arr_global = tb_global_create(m->mod, arr_name.len, cast(char const *)arr_name.text, cg_debug_type(m, array_type), TB_LINKAGE_PRIVATE);
		tb_global_set_storage(m->mod, tb_module_get_data(m->mod), arr_global,
		                      type_size_of(array_type), type_align_of(array_type), 0);
		cgValue all_tests_array = cg_value(tb_inst_get_symbol_address(p->func, cast(TB_Symbol *)arr_global),
		                                   alloc_type_pointer(array_type));

		isize testing_proc_index = 0;
		for (Entity *testing_proc : m->info->testing_procedures) {
			String test_name = testing_proc->token.string;

			String pkg_name = {};
			if (testing_proc->pkg != nullptr) {
				pkg_name = testing_proc->pkg->name;
			}
			cgValue v_pkg  = cg_const_string(p, t_string, pkg_name);
			cgValue v_name = cg_const_string(p, t_string, test_name);
			cgValue v_proc = cg_find_procedure_value_from_entity(m, testing_proc);

			cgValue elem = cg_emit_array_epi(p, all_tests_array, testing_proc_index++);
			cg_emit_store(p, cg_emit_struct_ep(p, elem, 0), v_pkg);
			cg_emit_store(p, cg_emit_struct_ep(p, elem, 1), v_name);
			cg_emit_store(p, cg_emit_struct_ep(p, elem, 2), v_proc);
		}

		cgAddr all_tests_slice = cg_add_local(p, slice_type, nullptr, true);
		cg_fill_slice(p, all_tests_slice,
		              cg_emit_array_epi(p, all_tests_array, 0),
		              cg_const_int(p, t_int, m->info->testing_procedures.count));

		cgValue runner = cg_find_package_value(m, str_lit("testing"), str_lit("runner"));

		TEMPORARY_ALLOCATOR_GUARD();
		auto args = slice_make<cgValue>(temporary_allocator(), 1);
		args[0] = cg_addr_load(p, all_tests_slice);
		cgValue result = cg_emit_call(p, runner, args);

		cgValue exit_runner = {};
		{
			AstPackage *pkg = get_runtime_package(m->info);

			String exit_name = str_lit("exit");
			Entity *e = scope_lookup_current(pkg->scope, string_interner_insert(exit_name));
			if (e == nullptr) {
				compiler_error("Could not find type declaration for '%.*s.%.*s'\n", LIT(pkg->name), LIT(exit_name));
			}
			exit_runner = cg_find_value_from_entity(m, e);
		}

		auto exit_args = slice_make<cgValue>(temporary_allocator(), 1);
		exit_args[0] = cg_emit_select(p, result, cg_const_int(p, t_int, 0), cg_const_int(p, t_int, 1));
		cg_emit_call(p, exit_runner, exit_args);
	}

	// no explicit terminator: runtime.exit is `-> !`, and cg_procedure_end
	// emits unreachable for any still-open block of a proc with results.
	cg_procedure_end(p);

	return p;
}

gb_internal bool cg_generate_code(Checker *c, LinkerData *linker_data) {
	TIME_SECTION("Tilde Module Initializtion");

	CheckerInfo *info = &c->info;

	linker_data_init(linker_data, info, c->parser->init_fullpath);

	#if defined(GB_SYSTEM_OSX)
		linker_enable_system_library_linking(linker_data);
	#endif
	cg_global_arena_init();

	cgModule *m = cg_module_create(c);
	defer (cg_module_destroy(m));

	m->linker_data = linker_data;
	for (Entity *e : info->required_foreign_imports_through_force) {
		cg_add_foreign_library_path(m, e);
	}

	m->do_threading = false;

	// NOTE(odin-tilde): establish the deterministic entity order before any
	// naming or emission happens (see entity_ranks/entity_ordinals).
	cg_build_deterministic_entity_order(m);

	TIME_SECTION("Tilde Global Variables");

	Array<cgGlobalVariable> global_variables = {};
	bool already_has_entry_point = cg_global_variables_create(m, &global_variables);

	if (true) {
		Type *proc_type = alloc_type_proc(nullptr, nullptr, 0, nullptr, 0, false, ProcCC_Odin);
		cgProcedure *p = cg_procedure_create_dummy(m, str_lit(CG_STARTUP_RUNTIME_PROC_NAME), proc_type, TB_LINKAGE_PRIVATE);
		p->is_startup = true;
		cg_startup_runtime_proc = p;
	}

	if (true) {
		Type *proc_type = alloc_type_proc(nullptr, nullptr, 0, nullptr, 0, false, ProcCC_Odin);
		cgProcedure *p = cg_procedure_create_dummy(m, str_lit(CG_CLEANUP_RUNTIME_PROC_NAME), proc_type, TB_LINKAGE_PRIVATE);
		p->is_startup = true;
		cg_cleanup_runtime_proc = p;
	}


	Array<cgProcedure *> procedures_to_generate = {};
	array_init(&procedures_to_generate, heap_allocator());
	defer (array_free(&procedures_to_generate));

	for (Entity *e : m->entities_sorted) {
		String name  = e->token.string;
		Scope *scope = e->scope;

		if ((scope->flags & ScopeFlag_File) == 0) {
			continue;
		}

		Scope *package_scope = scope->parent;
		GB_ASSERT(package_scope->flags & ScopeFlag_Pkg);

		if (e->kind != Entity_Procedure) {
			continue;
		}

		if (!(e->min_dep_count.load(std::memory_order_relaxed) > 0)) {
			// NOTE(bill): Nothing depends upon it so doesn't need to be built
			continue;
		}
		if (cgProcedure *p = cg_procedure_create(m, e)) {
			array_add(&procedures_to_generate, p);
		}
	}
	for (cgProcedure *p : procedures_to_generate) {
		cg_add_procedure_to_queue(p);
	}

	isize single_threaded_drained = 0;
	if (!m->do_threading) {
		for (isize i = 0; i < m->single_threaded_procedure_queue.count; i++) {
			cgProcedure *p = m->single_threaded_procedure_queue[i];
			cg_procedure_generate(p);
		}
		single_threaded_drained = m->single_threaded_procedure_queue.count;
	}

	thread_pool_wait();

	{
		cgProcedure *p = cg_startup_runtime_proc;
		// dummy proc has no Ast body; tb_builder_enter applies the prototype
		// itself, so skip cg_procedure_begin entirely.
		TB_GraphBuilder *g = tb_builder_enter(p->func, tb_module_get_text(p->module->mod), p->proto, nullptr);
		cg_tb_push_builder(p->func, g);
		// NOTE(odin-tilde): seed the context stack from the implicit context
		// parameter — @(init) procs with ProcCC_Odin need a caller context to
		// forward (cg_procedure_begin is skipped for this dummy proc).
		cg_push_context_onto_stack_from_implicit_parameter(p);
		cg_global_variables_initialize(p, &global_variables);
		// NOTE(odin-tilde): run @(init) procedures (os.init_std_files and
		// friends) exactly like the LLVM backend's startup runtime does.
		// Re-sorted for build determinism (see cg_init_fini_cmp).
		Array<Entity *> sorted_inits = array_make<Entity *>(temporary_allocator(), 0, info->init_procedures.count);
		for (Entity *e : info->init_procedures) {
			array_add(&sorted_inits, e);
		}
		array_sort(sorted_inits, cg_init_fini_cmp);
		for (Entity *e : sorted_inits) {
			cgValue value = cg_find_procedure_value_from_entity(m, e);
			cg_emit_call(p, value, {});
		}
		tb_inst_ret(p->func, 0, nullptr);
		cg_procedure_end(p);
	}
	{
		cgProcedure *p = cg_cleanup_runtime_proc;
		TB_GraphBuilder *g = tb_builder_enter(p->func, tb_module_get_text(p->module->mod), p->proto, nullptr);
		cg_tb_push_builder(p->func, g);
		// NOTE(odin-tilde): same implicit-context seeding as the startup proc.
		cg_push_context_onto_stack_from_implicit_parameter(p);
		// NOTE(odin-tilde): run @(fini) procedures like the LLVM backend
		// (reverse of init order), sorted for build determinism.
		Array<Entity *> sorted_finis = array_make<Entity *>(temporary_allocator(), 0, info->fini_procedures.count);
		for (Entity *e : info->fini_procedures) {
			array_add(&sorted_finis, e);
		}
		array_sort(sorted_finis, cg_init_fini_cmp);
		for (isize i = sorted_finis.count-1; i >= 0; i--) {
			Entity *e = sorted_finis[i];
			cgValue value = cg_find_procedure_value_from_entity(m, e);
			cg_emit_call(p, value, {});
		}
		tb_inst_ret(p->func, 0, nullptr);
		cg_procedure_end(p);
	}

	// NOTE(odin-tilde): `odin test` compiles the runtime's own main out
	// (ODIN_TEST), so synthesize the C entry point here — mirrors the LLVM
	// backend's "LLVM main" step. Built BEFORE the late-proc drain below so
	// that anything it lazily references (e.g. runtime.__init_context) is
	// still codegen'd in the single-threaded drain.
	if (build_context.command_kind == Command_test && !already_has_entry_point) {
		TIME_SECTION("Tilde main");
		cg_create_main_procedure(m, cg_startup_runtime_proc, cg_cleanup_runtime_proc);
	}

	// NOTE(odin-tilde): codegen of the startup/cleanup procs (global
	// initializer expressions, @(init)/@(fini) calls) can discover new
	// procedures — e.g. anonymous proc literals used as global proc values.
	// Drain anything queued after the main wait, otherwise those functions
	// are exported with no body (NULL TB_Function.output) and the x64
	// emitter dereferences it in emit_call_patches.
	if (!m->do_threading) {
		for (isize i = single_threaded_drained; i < m->single_threaded_procedure_queue.count; i++) {
			cgProcedure *p = m->single_threaded_procedure_queue[i];
			cg_procedure_generate(p);
		}
	}
	thread_pool_wait();


	// NOTE(macos): debug info export is not wired up yet (the Mach-O writer
	// only handles TB_DEBUGFMT_NONE), so we always export without debug info.
	TB_DebugFormat debug_format = TB_DEBUGFMT_NONE;
	TB_Arena *export_arena = cg_arena();
	TB_ExportBuffer export_buffer = tb_module_object_export(m->mod, export_arena, debug_format);
	// NOTE: the export buffer lives in the thread arena; no separate free.

	String filepath_obj = cg_filepath_obj_for_module(m, false);
	array_add(&linker_data->output_object_paths, filepath_obj);
	GB_ASSERT(tb_export_buffer_to_file(export_buffer, cast(char const *)filepath_obj.text));

	return true;
}

#undef ABI_PKG_NAME_SEPARATOR
