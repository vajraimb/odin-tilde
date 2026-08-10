gb_internal TB_DataType cg_gpr_blob_dt(i64 size) {
	if (size <= 1) return TB_TYPE_I8;
	if (size == 2) return TB_TYPE_I16;
	if (size <= 4) return TB_TYPE_I32;
	GB_ASSERT(size <= 8);
	return TB_TYPE_I64;
}

gb_internal i64 cg_gpr_blob_capacity(i64 size) {
	if (size <= 1) return 1;
	if (size == 2) return 2;
	if (size <= 4) return 4;
	GB_ASSERT(size <= 8);
	return 8;
}

gb_internal TB_Node *cg_load_gpr_blob(cgProcedure *p, TB_Node *addr, i64 size, i64 align) {
	TB_DataType transport = cg_gpr_blob_dt(size);
	// NOTE(odin-tilde): zero-sized types (empty struct/array fields, e.g. in
	// flags.parse option structs) classify TB_PASSING_DIRECT with a dummy I8
	// transport — the callee stores one blob byte into its (clamped 1-byte)
	// local, so the caller must produce a real node, not a null blob.
	if (size == 0) {
		return tb_inst_uint(p->func, transport, 0);
	}
	switch (size) {
	case 1: case 2: case 4: case 8:
		return tb_inst_load(p->func, transport, addr, cast(TB_CharUnits)align, false);
	}
	// NOTE(odin-tilde): odd sizes (3, 5, 6, 7) must be composed from chunks
	// that never read past `size` — a direct transport-width load would
	// over-read the object (potentially past a page boundary).
	TB_Node *blob = nullptr;
	i64 offset = 0;
	i64 remaining = size;
	while (remaining > 0) {
		TB_DataType cdt;
		i64 csize;
		if (remaining >= 4)      { cdt = TB_TYPE_I32; csize = 4; }
		else if (remaining >= 2) { cdt = TB_TYPE_I16; csize = 2; }
		else                     { cdt = TB_TYPE_I8;  csize = 1; }
		TB_Node *chunk_addr = offset ? tb_inst_member_access(p->func, addr, offset) : addr;
		TB_Node *chunk = tb_inst_load(p->func, cdt, chunk_addr, 1, false);
		chunk = tb_inst_zxt(p->func, chunk, transport);
		if (offset) {
			chunk = tb_inst_shl(p->func, chunk, tb_inst_uint(p->func, transport, cast(u64)(offset*8)), TB_ARITHMATIC_NONE);
		}
		blob = blob ? tb_inst_or(p->func, blob, chunk) : chunk;
		offset += csize;
		remaining -= csize;
	}
	return blob;
}

gb_internal TB_FunctionPrototype *cg_procedure_type_as_prototype(cgModule *m, Type *type) {
	GB_ASSERT(is_type_proc(type));
	mutex_lock(&m->proc_proto_mutex);
	defer (mutex_unlock(&m->proc_proto_mutex));

	if (type->kind == Type_Named) {
		type = base_type(type);
	}
	TB_FunctionPrototype **found = map_get(&m->proc_proto_map, type);
	if (found) {
		return *found;
	}

	TB_DebugType *dbg = cg_debug_type_for_proc(m, type);
	if (getenv("TILDE_PROTO_DEBUG")) { fprintf(stderr, "[tilde] mkproto %s\n", type_to_string(type)); fflush(stderr); }
	TB_FunctionPrototype *proto = tb_prototype_from_dbg(m->mod, dbg);

	map_set(&m->proc_proto_map, type, proto);
	return proto;
}

gb_internal cgProcedure *cg_procedure_create(cgModule *m, Entity *entity, bool ignore_body) {
	GB_ASSERT(entity != nullptr);
	GB_ASSERT(entity->kind == Entity_Procedure);
	if (!entity->Procedure.is_foreign) {
		if ((entity->flags & EntityFlag_ProcBodyChecked) == 0) {
			GB_PANIC("%.*s :: %s (was parapoly: %d %d)", LIT(entity->token.string), type_to_string(entity->type), is_type_polymorphic(entity->type, true), is_type_polymorphic(entity->type, false));
		}
	}

	String link_name = cg_get_entity_name(m, entity);

	cgProcedure *p = nullptr;
	{
		StringHashKey key = string_hash_string(link_name);
		cgValue *found = string_map_get(&m->members, key);
		if (found) {
			cg_add_entity(m, entity, *found);
			rw_mutex_lock(&m->values_mutex);
			p = string_map_must_get(&m->procedures, key);
			rw_mutex_unlock(&m->values_mutex);
			if (!ignore_body && p->func != nullptr) {
				return nullptr;
			}
		}
	}

	if (p == nullptr) {
		p = gb_alloc_item(permanent_allocator(), cgProcedure);
	}

	p->module = m;
	p->entity = entity;
	p->name = link_name;

	DeclInfo *decl = entity->decl_info;

	ast_node(pl, ProcLit, decl->proc_lit);
	Type *pt = base_type(entity->type);
	GB_ASSERT(pt->kind == Type_Proc);

	p->type           = entity->type;
	p->type_expr      = decl->type_expr;
	p->body           = pl->body;
	p->inlining       = pl->inlining;
	p->is_foreign     = entity->Procedure.is_foreign;
	p->is_export      = entity->Procedure.is_export;
	p->is_entry_point = false;
	p->split_returns_index = -1;

	if (p->is_foreign) {
		cg_add_foreign_library_path(m, entity->Procedure.foreign_library);
	}

	gbAllocator a = heap_allocator();
	p->children.allocator      = a;

	p->defer_stack.allocator   = a;
	p->scope_stack.allocator   = a;
	p->context_stack.allocator = a;

	p->control_regions.allocator = a;
	p->branch_regions.allocator = a;

	map_init(&p->variable_map);
	map_init(&p->soa_values_map);

	TB_Linkage linkage = TB_LINKAGE_PRIVATE;
	if (p->is_export) {
		linkage = TB_LINKAGE_PUBLIC;
	} else if (p->is_foreign || ignore_body) {
		if (ignore_body) {
			linkage = TB_LINKAGE_PUBLIC;
		}
		p->symbol = cast(TB_Symbol *)tb_extern_create(m->mod, link_name.len, cast(char const *)link_name.text, TB_EXTERNAL_SO_LOCAL);
	}
	if (p->name == "main") {
		// TODO(bill): figure out when this should be public or not
		linkage = TB_LINKAGE_PUBLIC;
	}

	if (p->symbol == nullptr)  {
		p->func = tb_function_create(m->mod, link_name.len, cast(char const *)link_name.text, linkage);

		if (cg_tb_verbose_codegen) {
			fprintf(stderr, "[tilde] proto %.*s\n", LIT(link_name));
		}
		if (getenv("TILDE_PROTO_DEBUG")) { fprintf(stderr, "[tilde] mktype %.*s :: %s\n", LIT(link_name), type_to_string(p->type)); fflush(stderr); }
		p->debug_type = cg_debug_type_for_proc(m, p->type);
		p->proto = tb_prototype_from_dbg(m->mod, p->debug_type);

		p->symbol = cast(TB_Symbol *)p->func;
	}

	p->value = cg_value(p->symbol, p->type);

	cg_add_symbol(m, entity, p->symbol);
	cg_add_entity(m, entity, p->value);
	cg_add_member(m, p->name, p->value);
	cg_add_procedure_value(m, p);


	return p;
}

gb_internal cgProcedure *cg_procedure_create_dummy(cgModule *m, String const &link_name, Type *type, TB_Linkage linkage) {
	auto *prev_found = string_map_get(&m->members, link_name);
	GB_ASSERT_MSG(prev_found == nullptr, "failed to create dummy procedure for: %.*s", LIT(link_name));

	cgProcedure *p = gb_alloc_item(permanent_allocator(), cgProcedure);

	p->module = m;
	p->name = link_name;

	p->type           = type;
	p->type_expr      = nullptr;
	p->body           = nullptr;
	p->tags           = 0;
	p->inlining       = ProcInlining_none;
	p->is_foreign     = false;
	p->is_export      = false;
	p->is_entry_point = false;
	p->split_returns_index = -1;

	gbAllocator a = heap_allocator();
	p->children.allocator      = a;

	p->defer_stack.allocator   = a;
	p->scope_stack.allocator   = a;
	p->context_stack.allocator = a;

	p->control_regions.allocator = a;
	p->branch_regions.allocator = a;

	map_init(&p->variable_map);
	map_init(&p->soa_values_map);

	// NOTE(new-tb): linkage must be fixed at creation time — TB's symbol hash
	// set keys PRIVATE symbols by pointer and PUBLIC ones by name, so mutating
	// linkage after interning corrupts the table (a resize can then visit the
	// symbol twice, emitting duplicate atoms and tripping ld's
	// "malformed atom files with duplicate names" assertion).
	p->func = tb_function_create(m->mod, link_name.len, cast(char const *)link_name.text, linkage);

	p->debug_type = cg_debug_type_for_proc(m, p->type);
	p->proto = tb_prototype_from_dbg(m->mod, p->debug_type);

	p->symbol = cast(TB_Symbol *)p->func;

	cgValue proc_value = cg_value(p->symbol, p->type);
	cg_add_member(m, p->name, proc_value);
	cg_add_procedure_value(m, p);

	return p;
}

gb_internal cgProcedure *cg_procedure_generate_anonymous(cgModule *m, Ast *expr, cgProcedure *parent) {
	expr = unparen_expr(expr);
	ast_node(pl, ProcLit, expr);

	mutex_lock(&m->anonymous_proc_lits_mutex);
	defer (mutex_unlock(&m->anonymous_proc_lits_mutex));

	cgProcedure **found = map_get(&m->anonymous_proc_lits_map, expr);
	if (found) {
		return *found;
	}

	TokenPos pos = ast_token(expr).pos;

	// NOTE(bill): Generate a new name
	// parent$count

	String name = cg_generated_name(m, parent, "anon");

	Type *type = type_of_expr(expr);

	GB_ASSERT(pl->decl->entity == nullptr);
	Token token = {};
	token.pos = ast_token(expr).pos;
	token.kind = Token_Ident;
	token.string = name;
	Entity *e = alloc_entity_procedure(nullptr, token, type, pl->tags);
	e->file = expr->file();

	// NOTE(bill): this is to prevent a race condition since these procedure literals can be created anywhere at any time
	e->decl_info = pl->decl;
	pl->decl->entity = e;
	e->flags |= EntityFlag_ProcBodyChecked;

	cgProcedure *p = cg_procedure_create(m, e);

	map_set(&m->anonymous_proc_lits_map, expr, p);

	if (parent != nullptr) {
		array_add(&parent->children, p);
	}

	cg_add_procedure_to_queue(p);
	return p;

}

gb_internal void cg_push_context_onto_stack_from_implicit_parameter(cgProcedure *p) {
	// NOTE(bill): Push context on to stack from implicit parameter
	GB_ASSERT(p->type->Proc.calling_convention == ProcCC_Odin);

	String name = str_lit("__.context_ptr");

	Entity *e = alloc_entity_param(nullptr, make_token_ident(name), t_context_ptr, false, false);
	e->flags |= EntityFlag_NoAlias;

	TB_Node *param_ptr = tb_inst_param(p->func, p->proto->param_count-1);
	cgValue local = cg_value(param_ptr, t_context_ptr);
	cgAddr addr = cg_addr(local);
	// NOTE(odin-tilde): the implicit context parameter must keep
	// cgAddr_Context kind (mirrors lb_push_context_onto_stack_from_implicit_parameter);
	// otherwise `context.x = v` stores bypass the copy-on-write
	// context machinery and clobber the caller-visible context
	// (inner scopes leaked writes to outer scopes).
	addr.kind = cgAddr_Context;
	map_set(&p->variable_map, e, addr);


	cgContextData *cd = array_add_and_get(&p->context_stack);
	cd->ctx = addr;
	cd->scope_index = -1;
	cd->uses = +1; // make sure it has been used already
}

gb_internal void cg_procedure_begin(cgProcedure *p) {
	if (p == nullptr || p->func == nullptr) {
		return;
	}

	TB_ModuleSectionHandle section = tb_module_get_text(p->module->mod);

	// NOTE(new-tb): declaration-only procs (foreign, no Ast body) never reach
	// here (cg_procedure_generate filters them), while dummy procs (equal /
	// hasher helpers) have no Ast body but DO emit a manual body, so they
	// must enter a builder like everyone else.
	TB_GraphBuilder *g = tb_builder_enter(p->func, section, p->proto, nullptr);
	cg_tb_push_builder(p->func, g);


	DeclInfo *decl = decl_info_of_entity(p->entity);
	if (decl != nullptr) {
		for_array(i, decl->labels) {
			BlockLabel bl = decl->labels[i];
			cgBranchRegions bb = {bl.label, nullptr, nullptr};
			array_add(&p->branch_regions, bb);
		}
	}

	GB_ASSERT(p->type->kind == Type_Proc);
	TypeProc *pt = &p->type->Proc;
	bool is_odin_like_cc = is_calling_convention_odin(pt->calling_convention);
	int param_index = 0;
	int param_count = p->proto->param_count;

	if (pt->results) {
		Type *result_type = nullptr;
		if (is_odin_like_cc) {
			result_type = pt->results->Tuple.variables[pt->results->Tuple.variables.count-1]->type;
		} else {
			result_type = reduce_tuple_to_single_type(pt->results);
		}
		TB_DebugType *debug_type = cg_debug_type(p->module, result_type);
		TB_PassingRule rule = tb_get_passing_rule_from_dbg(p->module->mod, debug_type, true);
		if (!is_odin_like_cc && cg_sysv_abi_active() && cg_sysv_needs_split(result_type)) {
			// NOTE(odin-tilde): SysV splits <= 16-byte C-boundary composites
			// into register chunks; they are not indirect (see tilde_abi.cpp)
			rule = TB_PASSING_DIRECT;
		}
		if (rule == TB_PASSING_INDIRECT) {
			p->return_by_ptr = true;
			param_index++;
		}
	}

	if (pt->params != nullptr) for (Entity *e : pt->params->Tuple.variables) {
		if (e->kind != Entity_Variable) {
			continue;
		}

		GB_ASSERT_MSG(param_index < param_count, "%d < %d %.*s :: %s", param_index, param_count, LIT(p->name), type_to_string(p->type));

		TB_Node *param_ptr = nullptr;

		TB_CharUnits size  = cast(TB_CharUnits)type_size_of(e->type);
		TB_CharUnits align = cast(TB_CharUnits)type_align_of(e->type);
		TB_DebugType *debug_type = cg_debug_type(p->module, e->type);
		TB_PassingRule rule = tb_get_passing_rule_from_dbg(p->module->mod, debug_type, false);
		cgSysVClass sysv_sc = {};
		bool sysv_split = false;
		if (!is_odin_like_cc && cg_sysv_abi_active() && cg_sysv_needs_split(e->type)) {
			// NOTE(odin-tilde): SysV per-eightbyte register chunks (tilde_abi.cpp)
			sysv_sc = cg_sysv_classify(e->type);
			sysv_split = true;
			rule = TB_PASSING_DIRECT;
		}
		switch (rule) {
		case TB_PASSING_DIRECT: {
			if (sysv_split && sysv_sc.n_chunks == 2) {
				// two register chunks (e.g. complex128 in xmm0:xmm1, u128 in
				// rdi:rsi): reassemble the aggregate in a callee-owned local
				param_ptr = tb_inst_local(p->func, size, align);
				for (int k = 0; k < 2; k++) {
					TB_Node *chunk = tb_inst_param(p->func, param_index++);
					TB_Node *dst = tb_inst_member_access(p->func, param_ptr, k*8);
					tb_inst_store(p->func, chunk->dt, dst, chunk, 1, false);
				}
				break;
			}
			// NOTE(odin-tilde): a 1-chunk SSE aggregate (e.g. struct{float,float})
			// arrives as a single F32/F64 node; the plain store below preserves
			// its bits in the local.
			TB_Node *param = tb_inst_param(p->func, param_index++);
			// NOTE(odin-tilde): odd-sized aggregates (3, 5, 6, 7 bytes) arrive
			// as a transport-width integer blob (I32/I64); the local must be
			// big enough for the full blob width, not just the type size.
			TB_CharUnits local_size = size <= 8 ? cast(TB_CharUnits)cg_gpr_blob_capacity(size) : size;
			param_ptr = tb_inst_local(p->func, local_size, align);
			tb_inst_store(p->func, param->dt, param_ptr, param, align, false);
		} break;
		case TB_PASSING_INDIRECT: {
			// NOTE(odin-tilde): copy the aggregate into a callee-owned local.
			// The caller passes a pointer to (possibly its own) memory, so
			// Odin pass-by-value semantics require a private copy; it also
			// makes the parameter's address a plain rematerializable local
			// instead of a param projection that must stay live in a
			// register across every call in the function.
			TB_Node *param = tb_inst_param(p->func, param_index++);
			param_ptr = tb_inst_local(p->func, size, align);
			if (size > 0) {
				tb_inst_memcpy(p->func, param_ptr, param, tb_inst_uint(p->func, TB_TYPE_INT, size), align);
			}
		} break;
		case TB_PASSING_IGNORE:
			continue;
		}

		GB_ASSERT(param_ptr->dt.type == TB_TAG_PTR);

		cgValue local = cg_value(param_ptr, alloc_type_pointer(e->type));

		if (e != nullptr && e->token.string.len > 0 && e->token.string != "_") {
			// NOTE(bill): for debugging purposes only
			String name = e->token.string;
			tb_function_attrib_variable(
				p->func,
				param_ptr,
				nullptr, // parent
				name.len, cast(char const *)name.text,
				debug_type
			);
		}
		cgAddr addr = cg_addr(local);
		if (e) {
			map_set(&p->variable_map, e, addr);
		}
	}

	if (is_odin_like_cc) {
		p->split_returns_index = param_index;
	}

	if (pt->calling_convention == ProcCC_Odin) {
		cg_push_context_onto_stack_from_implicit_parameter(p);
	}

	if (pt->has_named_results) {
		auto const &results = pt->results->Tuple.variables;
		for_array(i, results) {
			Entity *e = results[i];
			GB_ASSERT(e->kind == Entity_Variable);

			if (e->token.string == "") {
				continue;
			}
			GB_ASSERT(!is_blank_ident(e->token));

			cgAddr res = cg_add_local(p, e->type, e, true);

			if (e->Variable.param_value.kind != ParameterValue_Invalid) {
				cgValue c = cg_handle_param_value(p, e->type, e->Variable.param_value, e->token.pos);
				cg_addr_store(p, res, c);
			}
		}
	}
}


extern "C" bool tb_opt__REGALLOC;
extern "C" bool tb_opt__REGSPLIT;
extern "C" bool tb_opt_peeps_only(TB_Function *f, TB_Worklist *ws);

gb_internal WORKER_TASK_PROC(cg_procedure_compile_worker_proc) {
	cgProcedure *p = cast(cgProcedure *)data;

	// TEMP(debug): TILDE_SKIP="foo,bar" skips codegen for matching procs
	static gb_thread_local char const *skip_list = nullptr;
	static gb_thread_local bool skip_inited = false;
	if (!skip_inited) { skip_inited = true; skip_list = getenv("TILDE_SKIP"); }
	if (skip_list != nullptr) {
		char buf[4096];
		gb_strlcpy(buf, skip_list, gb_size_of(buf));
		for (char *tok = strtok(buf, ","); tok != nullptr; tok = strtok(nullptr, ",")) {
			if (strstr(cast(char const *)p->name.text, tok) != nullptr) {
				gb_printf_err("[tilde] SKIP %.*s\n", LIT(p->name));
				return 0;
			}
		}
	}

	// TEMP(debug): TILDE_RA_DEBUG=<substr> turns on TB's RA tracing for matching procs
	static gb_thread_local char const *ra_dbg_filter = nullptr;
	static gb_thread_local bool ra_dbg_inited = false;
	if (!ra_dbg_inited) { ra_dbg_inited = true; ra_dbg_filter = getenv("TILDE_RA_DEBUG"); }
	if (ra_dbg_filter != nullptr) {
		char namebuf[1024];
		isize nlen = gb_min(cast(isize)p->name.len, cast(isize)gb_size_of(namebuf)-1);
		gb_memmove(namebuf, p->name.text, nlen);
		namebuf[nlen] = 0;
		if (strstr(namebuf, ra_dbg_filter) != nullptr) {
			tb_opt__REGALLOC = true;
			tb_opt__REGSPLIT = true;
		}
	}

	// NOTE(new-tb): the old tb_pass_* driver is gone; optimization is
	// tb_opt(...) and machine-code emission is tb_codegen(...). We run the
	// peephole-only subset: local idioms (rotate recognition, etc.) and DCE,
	// without locals-to-SSA / loop opts / SLP (untested in this pipeline).
	TB_Worklist *ws = tb_worklist_alloc();
	defer (tb_worklist_free(ws));

	tb_opt_peeps_only(p->func, ws);

	bool emit_asm = false;
	static gb_thread_local char const *asm_filter = nullptr;
	static gb_thread_local bool asm_inited = false;
	if (!asm_inited) { asm_inited = true; asm_filter = getenv("TILDE_ASM"); }
	if (asm_filter != nullptr) {
		char namebuf[1024];
		isize nlen = gb_min(cast(isize)p->name.len, cast(isize)gb_size_of(namebuf)-1);
		gb_memmove(namebuf, p->name.text, nlen);
		namebuf[nlen] = 0;
		if (strstr(namebuf, asm_filter) != nullptr) {
			emit_asm = true;
		}
	}

	// emit ir
	static gb_thread_local char const *ir_filter = nullptr;
	static gb_thread_local bool ir_inited = false;
	if (!ir_inited) { ir_inited = true; ir_filter = getenv("TILDE_IR"); }
	if (ir_filter != nullptr) {
		char namebuf[1024];
		isize nlen = gb_min(cast(isize)p->name.len, cast(isize)gb_size_of(namebuf)-1);
		gb_memmove(namebuf, p->name.text, nlen);
		namebuf[nlen] = 0;
		if (strstr(namebuf, ir_filter) != nullptr) {
			tb_print(p->func);
			fprintf(stdout, "\n");
			fflush(stdout);
		}
	}

	// compile
	if (cg_tb_verbose_codegen) {
		gb_printf_err("[tilde] codegen %.*s\n", LIT(p->name));
	}
	TB_FunctionOutput *output = tb_codegen(p->func, TB_RA_ROGERS, ws, cg_arena(), emit_asm);
	if (emit_asm) {
		tb_output_print_asm(output, stdout);
		fprintf(stdout, "\n");
		fflush(stdout);
	}

	return 0;
}

gb_internal void cg_procedure_end(cgProcedure *p) {
	if (p == nullptr || p->func == nullptr) {
		return;
	}
	if (tb_inst_get_control(p->func)) {
		if (p->type->Proc.result_count == 0) {
			tb_inst_ret(p->func, 0, nullptr);
		} else {
			tb_inst_unreachable(p->func);
		}
	}

	// NOTE(new-tb): finish the graph builder for this body
	if (cg_tb_builder != nullptr && cg_tb_builder_func == p->func) {
		tb_builder_exit(cg_tb_builder);
	}
	cg_tb_pop_builder();

	if (p->module->do_threading) {
		thread_pool_add_task(cg_procedure_compile_worker_proc, p);
	} else {
		cg_procedure_compile_worker_proc(p);
	}
}

gb_internal void cg_procedure_generate(cgProcedure *p) {
	if (p->body == nullptr) {
		return;
	}

	cg_procedure_begin(p);
	cg_build_stmt(p, p->body);
	cg_procedure_end(p);
}

gb_internal void cg_build_nested_proc(cgProcedure *p, AstProcLit *pd, Entity *e) {
	GB_ASSERT(pd->body != nullptr);
	cgModule *m = p->module;

	if ((e->min_dep_count.load(std::memory_order_relaxed) > 0) == false) {
		// NOTE(bill): Nothing depends upon it so doesn't need to be built
		return;
	}

	// NOTE(bill): Generate a new name
	// parent.name-guid
	String original_name = e->token.string;
	String pd_name = original_name;
	if (e->Procedure.link_name.len > 0) {
		pd_name = e->Procedure.link_name;
	}


	isize name_len = p->name.len + 1 + pd_name.len + 1 + 10 + 1;
	char *name_text = gb_alloc_array(permanent_allocator(), char, name_len);

	i32 guid = cast(i32)p->children.count;
	name_len = gb_snprintf(name_text, name_len, "%.*s" ABI_PKG_NAME_SEPARATOR "%.*s-%d", LIT(p->name), LIT(pd_name), guid);
	String name = make_string(cast(u8 *)name_text, name_len-1);

	e->Procedure.link_name = name;

	cgProcedure *nested_proc = cg_procedure_create(p->module, e);
	e->cg_procedure = nested_proc;

	cgValue value = nested_proc->value;

	cg_add_entity(m, e, value);
	array_add(&p->children, nested_proc);
	cg_add_procedure_to_queue(nested_proc);
}





gb_internal cgValue cg_find_procedure_value_from_entity(cgModule *m, Entity *e) {
	GB_ASSERT(is_type_proc(e->type));
	e = strip_entity_wrapping(e);
	GB_ASSERT(e != nullptr);
	GB_ASSERT(e->kind == Entity_Procedure);

	cgValue *found = nullptr;
	rw_mutex_shared_lock(&m->values_mutex);
	found = map_get(&m->values, e);
	rw_mutex_shared_unlock(&m->values_mutex);
	if (found) {
		GB_ASSERT(found->node != nullptr);
		return *found;
	}

	// NOTE(new-tb): mirror cg_find_symbol's lazy path — runtime helpers
	// (e.g. __dynamic_map_get) referenced from generated helper procs may
	// not be in the minimum dependency set; generate them on demand
	// instead of panicking.
	if (cgProcedure *lazy = cg_procedure_create(m, e)) {
		cg_add_procedure_to_queue(lazy);
		rw_mutex_shared_lock(&m->values_mutex);
		found = map_get(&m->values, e);
		rw_mutex_shared_unlock(&m->values_mutex);
		if (found) {
			GB_ASSERT(found->node != nullptr);
			return *found;
		}
	}

	GB_PANIC("Error in: %s, missing procedure %.*s\n", token_pos_to_string(e->token.pos), LIT(e->token.string));
	return {};
}



gb_internal cgValue cg_build_call_expr_internal(cgProcedure *p, Ast *expr);

// PATCH(odin-tilde): #force_inline procs are inlined at the AST level. The
// LLVM backend gets this for free from the alwaysinline attribute (even at
// -O0); tilde has no inliner, so core crypto/bit helpers (SHA256_F1-F4 & co)
// stayed real calls — one of the last measurable runtime gaps. This is a
// deliberately restricted v1: the callee must be a checked, non-foreign,
// contextless (no implicit context rebinding), monomorphic (no type/const
// params), non-variadic proc whose body is a single `return <exprs>;`.
// Param entities are bound to the evaluated argument values in the caller's
// entity map and the return expression is built in the caller's TB function.
gb_internal bool cg_try_build_force_inline_call(cgProcedure *p, Ast *expr, Entity *proc_entity, cgValue *out) {
	ast_node(ce, CallExpr, expr);

	if (proc_entity == nullptr || proc_entity->kind != Entity_Procedure) return false;
	if (proc_entity->Procedure.is_foreign) return false;
	if ((proc_entity->flags & EntityFlag_ProcBodyChecked) == 0) return false;

	DeclInfo *decl = decl_info_of_entity(proc_entity);
	if (decl == nullptr || decl->proc_lit == nullptr) return false;
	ast_node(pl, ProcLit, decl->proc_lit);
	if (pl->inlining != ProcInlining_inline) return false; // #force_inline

	TypeProc *pt = &base_type(proc_entity->type)->Proc;
	if (pt->calling_convention != ProcCC_Contextless) return false;
	if (pt->variadic || pt->c_vararg) return false;
	if (ce->ellipsis.pos.line != 0) return false;
	if (ce->split_args == nullptr || ce->split_args->named.count != 0) return false;

	if (pl->body == nullptr || pl->body->kind != Ast_BlockStmt) return false;
	AstBlockStmt *body = &pl->body->BlockStmt;
	if (body->stmts.count != 1) return false;
	Ast *stmt = body->stmts[0];
	if (stmt->kind != Ast_ReturnStmt) return false;
	AstReturnStmt *rs = &stmt->ReturnStmt;

	TEMPORARY_ALLOCATOR_GUARD();

	auto params = array_make<Entity *>(temporary_allocator(), 0, pt->param_count);
	if (pt->params) for (Entity *e : pt->params->Tuple.variables) {
		if (e->kind != Entity_Variable) return false; // type/const param: polymorphic, skip v1
		array_add(&params, e);
	}
	if (params.count != ce->args.count) return false;

	auto results = array_make<Entity *>(temporary_allocator(), 0, pt->result_count);
	if (pt->results) for (Entity *e : pt->results->Tuple.variables) {
		array_add(&results, e);
	}
	if (results.count != rs->results.count) return false;
	if (results.count < 1 || results.count > 2) return false;
	for (Entity *e : results) {
		// union result conversion is ambiguous for some sources and can hit
		// the "TODO: src -> union" panic in cg_emit_conv; keep v1 simple
		if (is_type_union(base_type(e->type))) return false;
	}
	if (getenv("TILDE_INLINE_DEBUG")) {
		String n = proc_entity->token.string;
		gb_printf_err("[tilde-inline] %.*s\n", cast(int)n.len, n.text);
		for (Entity *e : results) {
			gb_printf_err("[tilde-inline]   result type: %s\n", type_to_string(e->type));
		}
	}

	// evaluate args once, spill each into a fresh local, and bind the param
	// entities to their addresses (idents resolve to lvalues, exactly like
	// regular proc parameters)
	for_array(i, params) {
		cgValue a = cg_build_expr(p, ce->args[i]);
		a = cg_emit_conv(p, a, params[i]->type);
		a = cg_flatten_value(p, a);
		cgAddr addr = cg_add_local(p, params[i]->type, params[i], false);
		cg_addr_store(p, addr, a);
	}

	if (results.count == 1) {
		cgValue r = cg_build_expr(p, rs->results[0]);
		r = cg_emit_conv(p, r, results[0]->type);
		// NOTE(odin-tilde): conv of untyped nil -> enum goes through
		// cg_const_nil, which re-types the zero to the enum's *core* type
		// (e.g. i32). Downstream conversions (enum -> union) then see the
		// core type and panic. Re-type same-size values to the named type.
		if (r.kind == cgValue_Value && r.type != results[0]->type &&
		    type_size_of(r.type) == type_size_of(results[0]->type)) {
			r = cg_emit_transmute(p, r, results[0]->type);
		}
		*out = r;
	} else {
		cgValue r0 = cg_emit_conv(p, cg_build_expr(p, rs->results[0]), results[0]->type);
		cgValue r1 = cg_emit_conv(p, cg_build_expr(p, rs->results[1]), results[1]->type);
		// NOTE(odin-tilde): conv of untyped nil -> enum goes through
		// cg_const_nil, which re-types the zero to the enum's *core* type
		// (e.g. i32). Downstream conversions (enum -> union) then see the
		// core type and panic. Re-type same-size values to the named type.
		if (r0.kind == cgValue_Value && r0.type != results[0]->type &&
		    type_size_of(r0.type) == type_size_of(results[0]->type)) {
			r0 = cg_emit_transmute(p, r0, results[0]->type);
		}
		if (r1.kind == cgValue_Value && r1.type != results[1]->type &&
		    type_size_of(r1.type) == type_size_of(results[1]->type)) {
			r1 = cg_emit_transmute(p, r1, results[1]->type);
		}
		*out = cg_value_multi2(r0, r1, type_of_expr(expr));
	}
	return true;
}

gb_internal cgValue cg_build_call_expr(cgProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);
	ast_node(ce, CallExpr, expr);

	cgValue res = cg_build_call_expr_internal(p, expr);

	if (ce->optional_ok_one) { // TODO(bill): Minor hack for #optional_ok procedures
		GB_ASSERT(res.kind == cgValue_Multi);
		GB_ASSERT(res.multi->values.count == 2);
		return res.multi->values[0];
	}
	return res;
}

gb_internal cgValue cg_emit_call(cgProcedure * p, cgValue value, Slice<cgValue> const &args) {
	if (value.kind == cgValue_Symbol) {
		value = cg_value(tb_inst_get_symbol_address(p->func, value.symbol), value.type);
	}
	GB_ASSERT(value.kind == cgValue_Value);
	TEMPORARY_ALLOCATOR_GUARD();

	TB_Module *m = p->module->mod;


	Type *type = base_type(value.type);
	GB_ASSERT(type->kind == Type_Proc);
	TypeProc *pt = &type->Proc;
	gb_unused(pt);

	TB_FunctionPrototype *proto = cg_procedure_type_as_prototype(p->module, type);
	TB_Node *target = value.node;

	// NOTE(odin-tilde): for c_vararg calls the individual vararg values are
	// appended after the fixed parameters, so the call can carry MORE params
	// than the prototype's formal count (which reserves exactly one slot for
	// the variadic entity). Size the param buffer for the actual arg count.
	isize params_capacity = proto->param_count;
	if (pt->c_vararg && pt->variadic) {
		isize fixed_count = 0;
		if (pt->params) for (Entity *e : pt->params->Tuple.variables) {
			if (e->kind == Entity_Variable) fixed_count += 1;
		}
		params_capacity += args.count - fixed_count;
	}
	auto params = slice_make<TB_Node *>(temporary_allocator(), params_capacity);


	// NOTE(new-tb): works for both Win64 and SystemV; TB classifies passing
	// rules from the debug types (tb_get_passing_rule_from_dbg).

	bool is_odin_like_cc = is_calling_convention_odin(pt->calling_convention);

	bool return_is_indirect = false;
	cgSysVClass sysv_return_sc = {};
	bool sysv_return_split = false;

	Slice<Entity *> result_entities = {};
	Slice<Entity *> param_entities  = {};
	if (pt->results) {
		result_entities = pt->results->Tuple.variables;
	}
	if (pt->params) {
		param_entities = pt->params->Tuple.variables;
	}

	isize param_index = 0;
	if (pt->result_count != 0) {
		Type *return_type = nullptr;
		if (is_odin_like_cc) {
			return_type = result_entities[result_entities.count-1]->type;
		} else {
			return_type = reduce_tuple_to_single_type(pt->results);
		}
		TB_DebugType *dbg = cg_debug_type(p->module, return_type);
		TB_PassingRule rule = tb_get_passing_rule_from_dbg(m, dbg, true);
		if (!is_odin_like_cc && cg_sysv_abi_active() && cg_sysv_needs_split(return_type)) {
			// NOTE(odin-tilde): SysV per-eightbyte register chunks (tilde_abi.cpp)
			sysv_return_sc = cg_sysv_classify(return_type);
			sysv_return_split = true;
			rule = TB_PASSING_DIRECT;
		}
		if (rule == TB_PASSING_INDIRECT) {
			return_is_indirect = true;
			TB_CharUnits size = cast(TB_CharUnits)type_size_of(return_type);
			TB_CharUnits align = cast(TB_CharUnits)gb_max(type_align_of(return_type), 16);
			TB_Node *local = tb_inst_local(p->func, size, align);
			tb_inst_memzero(p->func, local, tb_inst_uint(p->func, TB_TYPE_INT, size), align);
			params[param_index++] = local;
		}
	}
	isize param_entity_index = 0;
	for_array(i, args) {
		Entity *param_entity = nullptr;
		do {
			param_entity = param_entities[param_entity_index++];
		} while (param_entity->kind != Entity_Variable);
		Type *param_type = param_entity->type;
		bool is_c_vararg_arg = false;
		if (pt->c_vararg && pt->variadic && param_entity_index-1 == pt->variadic_index) {
			// NOTE(odin-tilde): c varargs — the individual vararg values are
			// appended after the fixed parameters, so keep consuming the
			// variadic entity for all remaining args.
			GB_ASSERT(param_type->kind == Type_Slice);
			param_entity_index--;
			is_c_vararg_arg = true;
		}
		cgValue arg = args[i];
		if (is_c_vararg_arg) {
			// NOTE(odin-tilde): mirror lb_emit_c_vararg — do NOT conv the
			// value to the variadic slice's element type. Converting an int
			// to `any` would box it into a 16-byte aggregate that travels
			// INDIRECT (as a pointer), so the callee (e.g. posix.fcntl)
			// reads a pointer value instead of the int. Each vararg keeps
			// its own (defaulted) type with the C default argument
			// promotions applied (f16/f32 -> f64, small ints/bools -> i32,
			// bit_set -> its backing integer).
			Type *elem_type = param_type->Slice.elem;
			Type *t = arg.type;
			if (!is_type_any(elem_type)) {
				t = elem_type;
				arg = cg_emit_conv(p, arg, t);
			}
			Type *core = core_type(t);
			if (core->kind == Type_BitSet) {
				core = core_type(bit_set_to_int(core));
				arg = cg_emit_transmute(p, arg, core);
			}
			param_type = c_vararg_promote_type(core);
			arg = cg_emit_conv(p, arg, param_type);
		} else {
			arg = cg_emit_conv(p, arg, param_type);
		}
		arg = cg_flatten_value(p, arg);

		TB_Node *param = nullptr;

		TB_DebugType *dbg = cg_debug_type(p->module, param_type);
		TB_PassingRule rule = tb_get_passing_rule_from_dbg(m, dbg, false);
		cgSysVClass sysv_sc = {};
		bool sysv_split = false;
		if (!is_odin_like_cc && cg_sysv_abi_active() && !is_c_vararg_arg && cg_sysv_needs_split(param_type)) {
			// NOTE(odin-tilde): SysV per-eightbyte register chunks (tilde_abi.cpp)
			sysv_sc = cg_sysv_classify(param_type);
			sysv_split = true;
			rule = TB_PASSING_DIRECT;
		}
		switch (rule) {
		case TB_PASSING_DIRECT: {
			if (sysv_split) {
				// emit one value per register chunk: SSE chunks are loaded
				// with a float dt (xmm transport), INTEGER chunks as an
				// integer blob (gpr transport); bits are preserved either way
				cgValue arg_ptr = cg_address_from_load_or_generate_local(p, arg);
				for (int k = 0; k < sysv_sc.n_chunks; k++) {
					TB_DataType cdt = cg_sysv_chunk_dt(sysv_sc, k);
					TB_Node *src = tb_inst_member_access(p->func, arg_ptr.node, k*8);
					TB_Node *chunk = tb_inst_load(p->func, cdt, src, 1, false);
					params[param_index++] = chunk;
				}
				continue;
			}
			if (arg.kind != cgValue_Value) {
				// NOTE(bill): small aggregates (<= 8 bytes on System-V)
				// are classified TB_PASSING_DIRECT and travel in a single
				// GPR as an integer blob, but aggregate rvalues live in
				// memory in this backend (cgValue_Addr), so load the blob
				// as an integer of the same size. The callee side stores
				// the parameter into a local with the same integer dt.
				// Odd sizes (3, 5, 6, 7) are composed from chunk loads that
				// never read past the object (see cg_load_gpr_blob).
				i64 sz = type_size_of(param_type);
				cgValue arg_ptr = cg_address_from_load_or_generate_local(p, arg);
				param = cg_load_gpr_blob(p, arg_ptr.node, sz, type_align_of(param_type));
			} else {
				param = arg.node;
			}
		} break;
		case TB_PASSING_INDIRECT:
			{
				cgValue arg_ptr = {};
				// indirect
				if (is_odin_like_cc) {
					arg_ptr = cg_address_from_load_or_generate_local(p, arg);
				} else {
					arg_ptr = cg_copy_value_to_ptr(p, arg, param_type, 16);
				}
				GB_ASSERT(arg_ptr.kind == cgValue_Value);
				param = arg_ptr.node;
			}
			break;
		case TB_PASSING_IGNORE:
			continue;
		}

		params[param_index++] = param;
	}

	// Split returns
	isize split_offset = -1;
	if (is_odin_like_cc) {
		split_offset = param_index;
		for (isize i = 0; i < pt->result_count-1; i++) {
			Type *result = result_entities[i]->type;
			TB_CharUnits size = cast(TB_CharUnits)type_size_of(result);
			TB_CharUnits align = cast(TB_CharUnits)gb_max(type_align_of(result), 16);
			TB_Node *local = tb_inst_local(p->func, size, align);
			// TODO(bill): Should this need to be zeroed any way?
			tb_inst_memzero(p->func, local, tb_inst_uint(p->func, TB_TYPE_INT, size), align);
			params[param_index++] = local;
		}
	}

	if (pt->calling_convention == ProcCC_Odin) {
		cgValue ctx_ptr = cg_find_or_generate_context_ptr(p).addr;
		GB_ASSERT(ctx_ptr.kind == cgValue_Value);
		params[param_index++] = ctx_ptr.node;
	}
	GB_ASSERT_MSG(param_index == params.count, "%td vs %td\n %s %u %u",
	              param_index, params.count,
	              type_to_string(type),
	              proto->return_count,
	              proto->param_count);

	for (TB_Node *param : params) {
		GB_ASSERT(param != nullptr);
	}

	GB_ASSERT(target != nullptr);
	TB_MultiOutput multi_output = tb_inst_call(p->func, proto, target, params.count, params.data);
	gb_unused(multi_output);

	switch (pt->result_count) {
	case 0:
		return {};
	case 1:
		if (sysv_return_split && sysv_return_sc.n_chunks == 2) {
			// NOTE(odin-tilde): two register chunks (e.g. complex128 in
			// xmm0:xmm1); reassemble the aggregate in a local
			GB_ASSERT(multi_output.count == 2);
			Type *rt = pt->results->Tuple.variables[0]->type;
			TB_CharUnits size  = cast(TB_CharUnits)type_size_of(rt);
			TB_CharUnits align = cast(TB_CharUnits)type_align_of(rt);
			TB_Node *addr = tb_inst_local(p->func, size, align);
			for (int k = 0; k < 2; k++) {
				TB_Node *chunk = multi_output.multiple[k];
				TB_Node *dst = tb_inst_member_access(p->func, addr, k*8);
				tb_inst_store(p->func, chunk->dt, dst, chunk, 1, false);
			}
			return cg_lvalue_addr(addr, rt);
		}
		if (return_is_indirect) {
			return cg_lvalue_addr(params[0], pt->results->Tuple.variables[0]->type);
		} else {
			GB_ASSERT(multi_output.count == 1);
			TB_Node *node = multi_output.single;
			Type *rt = pt->results->Tuple.variables[0]->type;
			TB_DataType dt = cg_data_type(rt);
			if (node->dt.raw != dt.raw) {
				// NOTE(new-tb): small aggregates returned DIRECT come back as
				// an integer register blob (same as the split-return path
				// below); store it into a local to reconstruct the aggregate
				// so downstream sees the usual address-based rvalue. The
				// local must fit the transport-width blob (odd sizes round up).
				TB_CharUnits size  = cast(TB_CharUnits)type_size_of(rt);
				TB_CharUnits align = cast(TB_CharUnits)type_align_of(rt);
				TB_Node *addr = tb_inst_local(p->func, size <= 8 ? cast(TB_CharUnits)cg_gpr_blob_capacity(size) : size, align);
				tb_inst_store(p->func, node->dt, addr, node, align, false);
				return cg_lvalue_addr(addr, rt);
			}
			return cg_value(node, rt);
		}
	}

	cgValueMulti *multi = gb_alloc_item(permanent_allocator(), cgValueMulti);
	multi->values = slice_make<cgValue>(permanent_allocator(), pt->result_count);

	if (is_odin_like_cc) {
		GB_ASSERT(split_offset >= 0);
		for (isize i = 0; i < pt->result_count-1; i++) {
			multi->values[i] = cg_lvalue_addr(params[split_offset+i], result_entities[i]->type);
		}

		Type *end_type = result_entities[pt->result_count-1]->type;
		if (return_is_indirect) {
			multi->values[pt->result_count-1] = cg_lvalue_addr(params[0], end_type);
		} else {
			GB_ASSERT(multi_output.count == 1);
			TB_DataType dt = cg_data_type(end_type);
			TB_Node *res = multi_output.single;
			if (res->dt.raw != dt.raw) {
				// struct-like returns passed in registers
				TB_CharUnits size  = cast(TB_CharUnits)type_size_of(end_type);
				TB_CharUnits align = cast(TB_CharUnits)type_align_of(end_type);
				TB_Node *addr = tb_inst_local(p->func, size <= 8 ? cast(TB_CharUnits)cg_gpr_blob_capacity(size) : size, align);
				tb_inst_store(p->func, res->dt, addr, res, align, false);
				multi->values[pt->result_count-1] = cg_lvalue_addr(addr, end_type);
			} else {
				multi->values[pt->result_count-1] = cg_value(res, end_type);
			}
		}
	} else {
		TB_Node *the_tuple = {};
		if (return_is_indirect) {
			the_tuple = params[0];
		} else {
			GB_ASSERT(multi_output.count == 1);
			TB_Node *res = multi_output.single;

			// struct-like returns passed in registers
			TB_CharUnits size  = cast(TB_CharUnits)type_size_of(pt->results);
			TB_CharUnits align = cast(TB_CharUnits)type_align_of(pt->results);
			the_tuple = tb_inst_local(p->func, size, align);
			tb_inst_store(p->func, res->dt, the_tuple, res, align, false);
		}
		for (isize i = 0; i < pt->result_count; i++) {
			i64 offset = type_offset_of(pt->results, i, nullptr);
			TB_Node *ptr = tb_inst_member_access(p->func, the_tuple, offset);
			multi->values[i] = cg_lvalue_addr(ptr, result_entities[i]->type);
		}
	}

	return cg_value_multi(multi, pt->results);
}

gb_internal cgValue cg_emit_runtime_call(cgProcedure *p, char const *name, Slice<cgValue> const &args) {
	AstPackage *pkg = p->module->info->runtime_package;
	u32 hash = 0;
	InternedString interned = string_interner_insert(make_string_c(name), 0, &hash);
	Entity *e = scope_lookup_current(pkg->scope, interned, hash);
	cgValue value = cg_find_procedure_value_from_entity(p->module, e);
	return cg_emit_call(p, value, args);
}

gb_internal cgValue cg_handle_param_value(cgProcedure *p, Type *parameter_type, ParameterValue const &param_value, TokenPos const &pos) {
	switch (param_value.kind) {
	case ParameterValue_Constant:
		if (is_type_constant_type(parameter_type)) {
			auto res = cg_const_value(p, parameter_type, param_value.value);
			return res;
		} else {
			ExactValue ev = param_value.value;
			cgValue arg = {};
			Type *type = type_of_expr(param_value.original_ast_expr);
			if (type != nullptr) {
				arg = cg_const_value(p, type, ev);
			} else {
				arg = cg_const_value(p, parameter_type, param_value.value);
			}
			return cg_emit_conv(p, arg, parameter_type);
		}

	case ParameterValue_Nil:
		return cg_const_nil(p, parameter_type);
	case ParameterValue_Location:
		{
			String proc_name = {};
			if (p->entity != nullptr) {
				proc_name = p->entity->token.string;
			}
			return cg_emit_source_code_location_as_global(p, proc_name, pos);
		}
	case ParameterValue_Value:
		return cg_build_expr(p, param_value.ast_value);
	}
	return cg_const_nil(p, parameter_type);
}

gb_internal cgValue cg_build_call_expr_internal(cgProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);

	TypeAndValue tv = type_and_value_of_expr(expr);

	TypeAndValue proc_tv = type_and_value_of_expr(ce->proc);
	AddressingMode proc_mode = proc_tv.mode;
	if (proc_mode == Addressing_Type) {
		GB_ASSERT(ce->args.count == 1);
		cgValue x = cg_build_expr(p, ce->args[0]);
		return cg_emit_conv(p, x, tv.type);
	}

	Ast *proc_expr = unparen_expr(ce->proc);
	if (proc_mode == Addressing_Builtin) {
		Entity *e = entity_of_node(proc_expr);
		BuiltinProcId id = BuiltinProc_Invalid;
		if (e != nullptr) {
			id = cast(BuiltinProcId)e->Builtin.id;
		} else {
			id = BuiltinProc_DIRECTIVE;
		}
		if (id == BuiltinProc___entry_point) {
			if (p->module->info->entry_point) {
				cgValue entry_point = cg_find_procedure_value_from_entity(p->module, p->module->info->entry_point);
				GB_ASSERT(entry_point.node != nullptr);
				cg_emit_call(p, entry_point, {});
			}
			return {};
		}

		return cg_build_builtin(p, id, expr);
	}

	// NOTE(bill): Regular call
	cgValue value = {};

	Entity *proc_entity = entity_of_node(proc_expr);
	if (proc_entity != nullptr) {
		if (proc_entity->flags & EntityFlag_Disabled) {
			GB_ASSERT(tv.type == nullptr);
			return {};
		}
	}

	// PATCH(odin-tilde): u128 arithmetic is not supported by this backend, so
	// math_bits.mul_u64 (which is defined via u128) is lowered directly to a
	// hardware widening multiply (TB_UMULPAIR -> RDX:RAX on x64).
	if (proc_entity != nullptr && proc_entity->kind == Entity_Procedure &&
	    proc_entity->pkg != nullptr && proc_entity->pkg->name == "math_bits" &&
	    proc_entity->token.string == "mul_u64" && ce->args.count == 2) {
		cgValue x = cg_flatten_value(p, cg_emit_conv(p, cg_build_expr(p, ce->args[0]), t_u64));
		cgValue y = cg_flatten_value(p, cg_emit_conv(p, cg_build_expr(p, ce->args[1]), t_u64));
		TB_Node *hi = nullptr;
		TB_Node *lo = tb_builder_umulpair(cg_tb_g(p->func), x.node, y.node, &hi);
		Type *res_type = type_of_expr(expr);
		GB_ASSERT(res_type->kind == Type_Tuple);
		GB_ASSERT(res_type->Tuple.variables.count == 2);
		return cg_value_multi2(cg_value(hi, t_u64), cg_value(lo, t_u64), res_type);
	}

	{
		cgValue inlined = {};
		if (cg_try_build_force_inline_call(p, expr, proc_entity, &inlined)) {
			return inlined;
		}
	}

	if (proc_expr->tav.mode == Addressing_Constant) {
		ExactValue v = proc_expr->tav.value;
		switch (v.kind) {
		case ExactValue_Integer:
			{
				u64 u = big_int_to_u64(&v.value_integer);
				cgValue x = cg_value(tb_inst_uint(p->func, TB_TYPE_PTR, u), t_rawptr);
				value = cg_emit_conv(p, x, proc_expr->tav.type);
				break;
			}
		case ExactValue_Pointer:
			{
				u64 u = cast(u64)v.value_pointer;
				cgValue x = cg_value(tb_inst_uint(p->func, TB_TYPE_PTR, u), t_rawptr);
				value = cg_emit_conv(p, x, proc_expr->tav.type);
				break;
			}
		}
	}

	if (value.node == nullptr) {
		value = cg_build_expr(p, proc_expr);
	}
	if (value.kind == cgValue_Addr) {
		value = cg_emit_load(p, value);
	}
	GB_ASSERT(value.kind == cgValue_Value);
	GB_ASSERT(value.node != nullptr);
	GB_ASSERT(is_type_proc(value.type));

	TEMPORARY_ALLOCATOR_GUARD();

	Type *proc_type_ = base_type(value.type);
	GB_ASSERT(proc_type_->kind == Type_Proc);
	TypeProc *pt = &proc_type_->Proc;

	GB_ASSERT(ce->split_args != nullptr);

	isize internal_param_count = 0;
	if (pt->params) for (Entity *e : pt->params->Tuple.variables) {
		if (e->kind == Entity_Variable) {
			internal_param_count += 1;
		}
	}
	GB_ASSERT(internal_param_count <= pt->param_count);

	auto args = array_make<cgValue>(temporary_allocator(), 0, internal_param_count);

	bool vari_expand = (ce->ellipsis.pos.line != 0);
	bool is_c_vararg = pt->c_vararg;

	for_array(i, ce->split_args->positional) {
		Entity *e = pt->params->Tuple.variables[i];
		if (e->kind == Entity_TypeName) {
			continue;
		} else if (e->kind == Entity_Constant) {
			continue;
		}

		GB_ASSERT(e->kind == Entity_Variable);
		if (pt->variadic && pt->variadic_index == i) {
			cgValue variadic_args = cg_const_nil(p, e->type);
			auto variadic = slice(ce->split_args->positional, pt->variadic_index, ce->split_args->positional.count);
			if (variadic.count != 0) {
				// variadic call argument generation
				Type *slice_type = e->type;
				GB_ASSERT(slice_type->kind == Type_Slice);

				if (is_c_vararg) {
					GB_ASSERT(!vari_expand);

					Type *elem_type = slice_type->Slice.elem;

					for (Ast *var_arg : variadic) {
						cgValue arg = cg_build_expr(p, var_arg);
						if (is_type_any(elem_type)) {
							// NOTE(odin-tilde): mirror lb — untyped nil varargs
							// become nil rawptr, not a conv to untyped nil.
							if (is_type_untyped_nil(arg.type)) {
								arg = cg_const_nil(p, t_rawptr);
							}
							array_add(&args, cg_emit_conv(p, arg, default_type(arg.type)));
						} else {
							array_add(&args, cg_emit_conv(p, arg, elem_type));
						}
					}
					break;
				} else if (vari_expand) {
					GB_ASSERT(variadic.count == 1);
					variadic_args = cg_build_expr(p, variadic[0]);
					variadic_args = cg_emit_conv(p, variadic_args, slice_type);
				} else {
					Type *elem_type = slice_type->Slice.elem;

					auto var_args = array_make<cgValue>(temporary_allocator(), 0, variadic.count);
					for (Ast *var_arg : variadic) {
						cgValue v = cg_build_expr(p, var_arg);
						cg_append_tuple_values(p, &var_args, v);
					}
					isize slice_len = var_args.count;
					if (slice_len > 0) {
						cgAddr slice = cg_add_local(p, slice_type, nullptr, true);
						cgAddr base_array = cg_add_local(p, alloc_type_array(elem_type, slice_len), nullptr, true);

						for (isize i = 0; i < var_args.count; i++) {
							cgValue addr = cg_emit_array_epi(p, base_array.addr, cast(i32)i);
							cgValue var_arg = var_args[i];
							var_arg = cg_emit_conv(p, var_arg, elem_type);
							cg_emit_store(p, addr, var_arg);
						}

						cgValue base_elem = cg_emit_array_epi(p, base_array.addr, 0);
						cgValue len = cg_const_int(p, t_int, slice_len);
						cg_fill_slice(p, slice, base_elem, len);

						variadic_args = cg_addr_load(p, slice);
					}
				}
			}
			array_add(&args, variadic_args);

			break;
		} else {
			cgValue value = cg_build_expr(p, ce->split_args->positional[i]);
			cg_append_tuple_values(p, &args, value);
		}
	}

	if (!is_c_vararg) {
		array_resize(&args, internal_param_count);
	}

	for (Ast *arg : ce->split_args->named) {
		ast_node(fv, FieldValue, arg);
		GB_ASSERT(fv->field->kind == Ast_Ident);
		String name = fv->field->Ident.token.string;
		gb_unused(name);
		isize param_index = lookup_procedure_parameter(pt, name);
		GB_ASSERT(param_index >= 0);

		cgValue value = cg_build_expr(p, fv->value);
		GB_ASSERT(!is_type_tuple(value.type));
		// NOTE(odin-tilde): `lookup_procedure_parameter` returns the index in
		// the FULL parameter tuple (including TypeName/Constant entries), but
		// `args` only holds slots for Entity_Variable parameters — the
		// positional loop skips non-Variable entries. Map the tuple index to
		// the variable ordinal. Hit by `make(Object, allocator=..., loc=...)`
		// whose callee is proc($T, Allocator, Source_Code_Location).
		isize var_index = 0;
		for (isize i = 0; i < param_index; i++) {
			Entity *pe = pt->params->Tuple.variables[i];
			if (pe->kind == Entity_Variable) {
				var_index += 1;
			}
		}
		GB_ASSERT(pt->params->Tuple.variables[param_index]->kind == Entity_Variable);
		args[var_index] = value;
	}

	TokenPos pos = ast_token(ce->proc).pos;


	if (pt->params != nullptr)  {
		isize min_count = internal_param_count;
		if (is_c_vararg) {
			min_count -= 1;
		}
		GB_ASSERT_MSG(args.count >= min_count, "in %.*s", LIT(p->name));
		isize arg_index = 0;
		for_array(param_index, pt->params->Tuple.variables) {
			Entity *e = pt->params->Tuple.variables[param_index];
			if (e->kind == Entity_TypeName) {
				continue;
			} else if (e->kind == Entity_Constant) {
				continue;
			}
			GB_ASSERT(e->kind == Entity_Variable);

			if (pt->variadic && param_index == pt->variadic_index) {
				// NOTE(odin-tilde): mirror lb — the variadic parameter is never
				// conv'd to its slice type. For !c_vararg it already holds the
				// packed slice (or nil when missing); for c_vararg the varargs
				// were appended individually and pass through untouched.
				if (!is_c_vararg && args[arg_index].node == nullptr) {
					args[arg_index++] = cg_const_nil(p, e->type);
					continue;
				}
				arg_index++;
				continue;
			}

			cgValue arg = args[arg_index];
			if (arg.node == nullptr) {
				GB_ASSERT(e->kind == Entity_Variable);
				args[arg_index++] = cg_handle_param_value(p, e->type, e->Variable.param_value, pos);
			} else {
				args[arg_index++] = cg_emit_conv(p, arg, e->type);
			}
		}
	}

	isize final_count = is_c_vararg ? args.count : internal_param_count;
	auto call_args = slice(args, 0, final_count);

	cgValue result = cg_emit_call(p, value, call_args);

	// NOTE(odin-tilde): @(deferred_in/out=...) procedure attributes — mirrors
	// lb_build_call_expr: after the call, register the associated deferred
	// procedure on the caller's current scope so it runs at scope exit
	// (e.g. sync.mutex_guard registering mutex_unlock).
	if (proc_entity != nullptr && proc_entity->kind == Entity_Procedure &&
	    entity_has_deferred_procedure(proc_entity)) {
		DeferredProcedureKind kind = proc_entity->Procedure.deferred_procedure.kind;
		Entity *deferred_entity = proc_entity->Procedure.deferred_procedure.entity;
		cgValue deferred = cg_find_procedure_value_from_entity(p->module, deferred_entity);

		bool by_ptr = false;
		Array<cgValue> result_as_args = {};
		switch (kind) {
		case DeferredProcedure_none:
			break;
		case DeferredProcedure_in_by_ptr:
			by_ptr = true;
			/*fallthrough*/
		case DeferredProcedure_in:
			result_as_args = array_make<cgValue>(heap_allocator(), call_args.count);
			for_array(i, call_args) {
				result_as_args[i] = call_args[i];
			}
			break;
		case DeferredProcedure_out_by_ptr:
			by_ptr = true;
			/*fallthrough*/
		case DeferredProcedure_out:
			result_as_args = array_make<cgValue>(heap_allocator(), 0, 2);
			cg_append_tuple_values(p, &result_as_args, result);
			break;
		case DeferredProcedure_in_out_by_ptr:
			by_ptr = true;
			/*fallthrough*/
		case DeferredProcedure_in_out:
			{
				result_as_args = array_make<cgValue>(heap_allocator(), 0, call_args.count+2);
				for_array(i, call_args) {
					array_add(&result_as_args, call_args[i]);
				}
				cg_append_tuple_values(p, &result_as_args, result);
			}
			break;
		}
		if (by_ptr) {
			for_array(i, result_as_args) {
				cgValue arg_ptr = cg_address_from_load_or_generate_local(p, result_as_args[i]);
				result_as_args[i] = arg_ptr;
			}
		}

		if (kind != DeferredProcedure_none) {
			cg_add_defer_proc(p, p->scope_index, deferred, slice(result_as_args, 0, result_as_args.count));
		}
	}

	return result;
}



gb_internal cgValue cg_hasher_proc_value_for_type(cgProcedure *p, Type *type) {
	cgProcedure *found = cg_hasher_proc_for_type(p->module, type);
	return cg_value(tb_inst_get_symbol_address(p->func, found->symbol), found->type);
}

gb_internal cgValue cg_equal_proc_value_for_type(cgProcedure *p, Type *type) {
	cgProcedure *found = cg_equal_proc_for_type(p->module, type);
	return cg_value(tb_inst_get_symbol_address(p->func, found->symbol), found->type);
}



gb_internal cgProcedure *cg_equal_proc_for_type(cgModule *m, Type *type) {
	type = base_type(type);
	GB_ASSERT(is_type_comparable(type));

	mutex_lock(&m->generated_procs_mutex);
	defer (mutex_unlock(&m->generated_procs_mutex));

	cgProcedure **found = map_get(&m->equal_procs, type);
	if (found) {
		return *found;
	}

	String proc_name = cg_typed_generated_name("__$equal", type);

	// NOTE(odin-tilde): dedup structurally identical twins by name (see
	// cgModule.generated_procs_by_name).
	if (cgProcedure **by_name = string_map_get(&m->generated_procs_by_name, string_hash_string(proc_name))) {
		map_set(&m->equal_procs, type, *by_name);
		return *by_name;
	}

	cgProcedure *p = cg_procedure_create_dummy(m, proc_name, t_equal_proc, TB_LINKAGE_PRIVATE);
	string_map_set(&m->generated_procs_by_name, proc_name, p);
	map_set(&m->equal_procs, type, p);
	p->split_returns_index = 2;

	cg_procedure_begin(p);

	TB_Node *x = tb_inst_param(p->func, 0);
	TB_Node *y = tb_inst_param(p->func, 1);
	GB_ASSERT(x->dt.type == TB_TAG_PTR);
	GB_ASSERT(y->dt.type == TB_TAG_PTR);

	TB_DataType ret_dt = TB_PROTOTYPE_RETURNS(p->proto)->dt;

	TB_Node *node_true  = tb_inst_uint(p->func, ret_dt, true);
	TB_Node *node_false = tb_inst_uint(p->func, ret_dt, false);

	TB_Node *same_ptr_region = cg_control_region(p, "same_ptr");
	TB_Node *diff_ptr_region = cg_control_region(p, "diff_ptr");

	TB_Node *is_same_ptr = tb_inst_cmp_eq(p->func, x, y);
	tb_inst_if(p->func, is_same_ptr, same_ptr_region, diff_ptr_region);

	tb_inst_set_control(p->func, same_ptr_region);
	tb_inst_ret(p->func, 1, &node_true);

	tb_inst_set_control(p->func, diff_ptr_region);

	Type *pt = alloc_type_pointer(type);
	cgValue lhs = cg_value(x, pt);
	cgValue rhs = cg_value(y, pt);

	if (type->kind == Type_Struct) {
		type_set_offsets(type);

		TB_Node *false_region  = cg_control_region(p, "bfalse");
		cgValue res = cg_const_bool(p, t_bool, true);

		for_array(i, type->Struct.fields) {
			TB_Node *next_region = cg_control_region(p, "btrue");

			cgValue plhs  = cg_emit_struct_ep(p, lhs, i);
			cgValue prhs  = cg_emit_struct_ep(p, rhs, i);
			cgValue left  = cg_emit_load(p, plhs);
			cgValue right = cg_emit_load(p, prhs);
			cgValue ok    = cg_emit_comp(p, Token_CmpEq, left, right);

			cg_emit_if(p, ok, next_region, false_region);

			cg_emit_goto(p, next_region);
			tb_inst_set_control(p->func, next_region);
		}

		tb_inst_ret(p->func, 1, &node_true);
		tb_inst_set_control(p->func, false_region);
		tb_inst_ret(p->func, 1, &node_false);

	} else if (type->kind == Type_Union) {
		if (type_size_of(type) == 0) {
			tb_inst_ret(p->func, 1, &node_true);
		} else if (is_type_union_maybe_pointer(type)) {
			Type *v = type->Union.variants[0];
			Type *pv = alloc_type_pointer(v);

			cgValue left  = cg_emit_load(p, cg_emit_conv(p, lhs, pv));
			cgValue right = cg_emit_load(p, cg_emit_conv(p, rhs, pv));
			cgValue ok = cg_emit_comp(p, Token_CmpEq, left, right);
			cg_build_return_stmt_internal_single(p, ok);
		} else {
			TB_Node *false_region  = cg_control_region(p, "bfalse");
			TB_Node *switch_region = cg_control_region(p, "bswitch");

			cgValue lhs_tag = cg_emit_load(p, cg_emit_union_tag_ptr(p, lhs));
			cgValue rhs_tag = cg_emit_load(p, cg_emit_union_tag_ptr(p, rhs));

			cgValue tag_eq = cg_emit_comp(p, Token_CmpEq, lhs_tag, rhs_tag);
			cg_emit_if(p, tag_eq, switch_region, false_region);

			size_t entry_count = type->Union.variants.count;
			if (type->Union.kind != UnionType_no_nil) {
				entry_count += 1;
			}

			size_t entry_offset = 0;

			TB_SwitchEntry *keys = gb_alloc_array(temporary_allocator(), TB_SwitchEntry, entry_count);
			if (type->Union.kind != UnionType_no_nil) {
				TB_Node *region = cg_control_region(p, "bcase");
				keys[entry_offset].key   = 0;
				keys[entry_offset].value = region;
				entry_offset += 1;
			}
			for (isize i = 0; i < type->Union.variants.count; i++) {
				TB_Node *region = cg_control_region(p, "bcase");
				Type *variant = type->Union.variants[i];
				keys[entry_offset+i].key = union_variant_index_checked(type, variant);
				keys[entry_offset+i].value = region;
			}

			// NOTE(odin-tilde): the switch must be emitted BEFORE the case
			// bodies — tb_builder_label_set nulls the builder's current
			// block when the target region has no predecessors, so entering
			// a case region before the branch registers its edge leaves the
			// builder with no control block and the next emission segfaults.
			tb_inst_set_control(p->func, switch_region);
			TB_DataType tag_dt = cg_data_type(lhs_tag.type);
			GB_ASSERT(lhs_tag.kind == cgValue_Value);
			tb_inst_branch(p->func, tag_dt, lhs_tag.node, false_region, entry_count, keys);

			if (type->Union.kind != UnionType_no_nil) {
				tb_inst_set_control(p->func, keys[0].value);
				cgValue ok = cg_const_bool(p, t_bool, true);
				cg_build_return_stmt_internal_single(p, ok);
			}
			for (isize i = 0; i < type->Union.variants.count; i++) {
				tb_inst_set_control(p->func, keys[entry_offset+i].value);
				Type *variant = type->Union.variants[i];
				Type *vp = alloc_type_pointer(variant);
				cgValue left  = cg_emit_load(p, cg_emit_conv(p, lhs, vp));
				cgValue right = cg_emit_load(p, cg_emit_conv(p, rhs, vp));
				cgValue ok = cg_emit_comp(p, Token_CmpEq, left, right);
				cg_build_return_stmt_internal_single(p, ok);
			}

			tb_inst_set_control(p->func, false_region);
			tb_inst_ret(p->func, 1, &node_false);
		}
	} else {
		cgValue left  = cg_lvalue_addr(x, type);
		cgValue right = cg_lvalue_addr(y, type);
		cgValue ok = cg_emit_comp(p, Token_CmpEq, left, right);
		cg_build_return_stmt_internal_single(p, ok);
	}

	cg_procedure_end(p);

	return p;
}


gb_internal cgValue cg_simple_compare_hash(cgProcedure *p, Type *type, cgValue data, cgValue seed) {
	TEMPORARY_ALLOCATOR_GUARD();

	GB_ASSERT_MSG(is_type_simple_compare(type), "%s", type_to_string(type));

	auto args = slice_make<cgValue>(temporary_allocator(), 3);
	args[0] = data;
	args[1] = seed;
	args[2] = cg_const_int(p, t_int, type_size_of(type));
	return cg_emit_runtime_call(p, "default_hasher", args);
}





gb_internal cgProcedure *cg_hasher_proc_for_type(cgModule *m, Type *type) {
	type = base_type(type);
	GB_ASSERT(is_type_valid_for_keys(type));

	mutex_lock(&m->generated_procs_mutex);
	defer (mutex_unlock(&m->generated_procs_mutex));

	cgProcedure **found = map_get(&m->hasher_procs, type);
	if (found) {
		return *found;
	}

	String proc_name = cg_typed_generated_name("__$hasher", type);

	// NOTE(odin-tilde): dedup structurally identical twins by name (see
	// cgModule.generated_procs_by_name).
	if (cgProcedure **by_name = string_map_get(&m->generated_procs_by_name, string_hash_string(proc_name))) {
		map_set(&m->hasher_procs, type, *by_name);
		return *by_name;
	}

	cgProcedure *p = cg_procedure_create_dummy(m, proc_name, t_hasher_proc, TB_LINKAGE_PRIVATE);
	string_map_set(&m->generated_procs_by_name, proc_name, p);
	map_set(&m->hasher_procs, type, p);
	p->split_returns_index = 2;

	cg_procedure_begin(p);
	defer (cg_procedure_end(p));

	TB_Node *x = tb_inst_param(p->func, 0); // data
	TB_Node *y = tb_inst_param(p->func, 1); // seed

	cgValue data = cg_value(x, t_rawptr);
	cgValue seed = cg_value(y, t_uintptr);

	if (is_type_simple_compare(type)) {
		cgValue res = cg_simple_compare_hash(p, type, data, seed);
		cg_build_return_stmt_internal_single(p, res);
		return p;
	}

	TEMPORARY_ALLOCATOR_GUARD();

	auto args = slice_make<cgValue>(temporary_allocator(), 2);

	if (type->kind == Type_Struct) {
		type_set_offsets(type);
		for_array(i, type->Struct.fields) {
			i64 offset = type->Struct.offsets[i];
			Entity *field = type->Struct.fields[i];
			cgValue field_hasher = cg_hasher_proc_value_for_type(p, field->type);

			TB_Node *ptr = tb_inst_member_access(p->func, data.node, offset);

			args[0] = cg_value(ptr, alloc_type_pointer(field->type));
			args[1] = seed;
			seed = cg_emit_call(p, field_hasher, args);
		}

		cg_build_return_stmt_internal_single(p, seed);
	} else if (type->kind == Type_Union) {
		if (type_size_of(type) == 0) {
			cg_build_return_stmt_internal_single(p, seed);
		} else if (is_type_union_maybe_pointer(type)) {
			Type *v = type->Union.variants[0];
			cgValue variant_hasher = cg_hasher_proc_value_for_type(p, v);

			args[0] = data;
			args[1] = seed;
			cgValue res = cg_emit_call(p, variant_hasher, args);
			cg_build_return_stmt_internal_single(p, res);
		} else {
			TB_Node *end_region = cg_control_region(p, "bend");
			TB_Node *switch_region = cg_control_region(p, "bswitch");

			cg_emit_goto(p, switch_region);

			size_t entry_count = type->Union.variants.count;
			TB_SwitchEntry *keys = gb_alloc_array(temporary_allocator(), TB_SwitchEntry, entry_count);
			for (size_t i = 0; i < entry_count; i++) {
				TB_Node *region = cg_control_region(p, "bcase");
				Type *variant = type->Union.variants[i];
				keys[i].key = union_variant_index_checked(type, variant);
				keys[i].value = region;

				tb_inst_set_control(p->func, region);

				cgValue variant_hasher = cg_hasher_proc_value_for_type(p, variant);

				args[0] = data;
				args[1] = seed;
				cgValue res = cg_emit_call(p, variant_hasher, args);
				cg_build_return_stmt_internal_single(p, res);
			}

			tb_inst_set_control(p->func, switch_region);

			cgValue tag_ptr = cg_emit_union_tag_ptr(p, data);
			cgValue tag = cg_emit_load(p, tag_ptr);

			TB_DataType tag_dt = cg_data_type(tag.type);
			GB_ASSERT(tag.kind == cgValue_Value);
			tb_inst_branch(p->func, tag_dt, tag.node, end_region, entry_count, keys);

			tb_inst_set_control(p->func, end_region);
			cg_build_return_stmt_internal_single(p, seed);
		}
	} else if (type->kind == Type_Array) {
		cgAddr pres = cg_add_local(p, t_uintptr, nullptr, false);
		cg_addr_store(p, pres, seed);

		cgValue elem_hasher = cg_hasher_proc_value_for_type(p, type->Array.elem);

		auto loop_data = cg_loop_start(p, type->Array.count, t_int);

		i64 stride = type_size_of(type->Array.elem);
		TB_Node *ptr = tb_inst_array_access(p->func, data.node, loop_data.index.node, stride);
		args[0] = cg_value(ptr, alloc_type_pointer(type->Array.elem));
		args[1] = cg_addr_load(p, pres);

		cgValue new_seed = cg_emit_call(p, elem_hasher, args);
		cg_addr_store(p, pres, new_seed);

		cg_loop_end(p, loop_data);

		cgValue res = cg_addr_load(p, pres);
		cg_build_return_stmt_internal_single(p, res);
	} else if (type->kind == Type_EnumeratedArray) {
		cgAddr pres = cg_add_local(p, t_uintptr, nullptr, false);
		cg_addr_store(p, pres, seed);

		cgValue elem_hasher = cg_hasher_proc_value_for_type(p, type->EnumeratedArray.elem);

		auto loop_data = cg_loop_start(p, type->EnumeratedArray.count, t_int);

		i64 stride = type_size_of(type->EnumeratedArray.elem);
		TB_Node *ptr = tb_inst_array_access(p->func, data.node, loop_data.index.node, stride);
		args[0] = cg_value(ptr, alloc_type_pointer(type->EnumeratedArray.elem));
		args[1] = cg_addr_load(p, pres);

		cgValue new_seed = cg_emit_call(p, elem_hasher, args);
		cg_addr_store(p, pres, new_seed);

		cg_loop_end(p, loop_data);

		cgValue res = cg_addr_load(p, pres);
		cg_build_return_stmt_internal_single(p, res);
	} else if (is_type_cstring(type)) {
		args[0] = data;
		args[1] = seed;
		cgValue res = cg_emit_runtime_call(p, "default_hasher_cstring", args);
		cg_build_return_stmt_internal_single(p, res);
	} else if (is_type_string(type)) {
		args[0] = data;
		args[1] = seed;
		cgValue res = cg_emit_runtime_call(p, "default_hasher_string", args);
		cg_build_return_stmt_internal_single(p, res);
	} else {
		GB_PANIC("Unhandled type for hasher: %s", type_to_string(type));
	}
	return p;
}