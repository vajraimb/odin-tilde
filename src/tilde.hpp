#if defined(GB_SYSTEM_WINDOWS)
	#pragma warning(push)
	#pragma warning(disable: 4200)
	#pragma warning(disable: 4201)
#endif

// tb.h uses C99 'restrict' in prototypes; map it for C++ on all platforms
#if defined(__cplusplus) && !defined(restrict)
	#define restrict gb_restrict
	#define CG_TB_UNDEF_RESTRICT 1
#endif

#include "tilde/tb.h"

#if defined(CG_TB_UNDEF_RESTRICT)
	#undef restrict
	#undef CG_TB_UNDEF_RESTRICT
#endif

#include "tilde/tb_compat.h"

#include <setjmp.h> // for TB RA bail / per-function peeps-only retry in the compile worker


#define TB_TYPE_F16    TB_TYPE_F16_COMPAT
#define TB_TYPE_I128   TB_TYPE_I128_COMPAT
#define TB_TYPE_INT    TB_TYPE_INTN(cast(u16)(8*build_context.int_size))
#define TB_TYPE_INTPTR TB_TYPE_INTN(cast(u16)(8*build_context.ptr_size))

#if defined(GB_SYSTEM_WINDOWS)
	#pragma warning(pop)
#endif

#define CG_STARTUP_RUNTIME_PROC_NAME   "__$startup_runtime"
#define CG_CLEANUP_RUNTIME_PROC_NAME   "__$cleanup_runtime"
#define CG_STARTUP_TYPE_INFO_PROC_NAME "__$startup_type_info"
#define CG_TYPE_INFO_DATA_NAME         "__$type_info_data"
#define CG_TYPE_INFO_TYPES_NAME        "__$type_info_types_data"
#define CG_TYPE_INFO_NAMES_NAME        "__$type_info_names_data"
#define CG_TYPE_INFO_OFFSETS_NAME      "__$type_info_offsets_data"
#define CG_TYPE_INFO_USINGS_NAME       "__$type_info_usings_data"
#define CG_TYPE_INFO_TAGS_NAME         "__$type_info_tags_data"
#define CG_TYPE_INFO_ENUM_VALUES_NAME  "__$type_info_enum_values_data"

struct cgModule;


enum cgValueKind : u32 {
	cgValue_Value,  // rvalue
	cgValue_Addr,   // lvalue
	cgValue_Symbol, // global
	cgValue_Multi,  // multiple values
};

struct cgValueMulti;

struct cgValue {
	cgValueKind kind;
	Type *      type;
	union {
		// NOTE: any value in this union must be a pointer
		TB_Symbol *   symbol;
		TB_Node *     node;
		cgValueMulti *multi;
	};
};

struct cgValueMulti {
	Slice<cgValue> values;
};


enum cgAddrKind {
	cgAddr_Default,
	cgAddr_Map,
	cgAddr_Context,
	cgAddr_SoaVariable,
	cgAddr_BitField,

	cgAddr_RelativePointer,
	cgAddr_RelativeSlice,

	cgAddr_Swizzle,
	cgAddr_SwizzleLarge,
};

struct cgAddr {
	cgAddrKind kind;
	cgValue addr;
	union {
		struct {
			cgValue key;
			Type *type;
			Type *result;
		} map;
		struct {
			Selection sel;
		} ctx;
		struct {
			cgValue index;
			Ast *index_expr;
		} soa;
		struct {
			cgValue index;
			Ast *node;
		} index_set;
		struct {
			bool deref;
		} relative;
		struct {
			Type *type;
			u8 count;      // 2, 3, or 4 components
			u8 indices[4];
		} swizzle;
		struct {
			Type *type;
			Slice<i32> indices;
		} swizzle_large;
		struct {
			Type *type;
			i64   bit_offset;
			u8    bit_size;
		} bitfield;
	};
};


struct cgTargetList {
	cgTargetList *prev;
	bool          is_block;
	// control regions
	TB_Node *     break_;
	TB_Node *     continue_;
	TB_Node *     fallthrough_;
};

struct cgBranchRegions {
	Ast *    label;
	TB_Node *break_;
	TB_Node *continue_;
};

enum cgDeferExitKind {
	cgDeferExit_Default,
	cgDeferExit_Return,
	cgDeferExit_Branch,
};

enum cgDeferKind {
	cgDefer_Node,
	cgDefer_Proc,
};

struct cgDefer {
	cgDeferKind kind;
	isize       scope_index;
	isize       context_stack_count;
	TB_Node *   control_region;
	union {
		Ast *stmt;
		struct {
			cgValue deferred;
			Slice<cgValue> result_as_args;
		} proc;
	};
};


struct cgContextData {
	cgAddr ctx;
	isize scope_index;
	isize uses;
};

struct cgControlRegion {
	TB_Node *control_region;
	isize    scope_index;
};

struct cgProcedure {
	u32 flags;
	u16 state_flags;

	cgProcedure *parent;
	Array<cgProcedure *> children;

	TB_Function *func;
	TB_FunctionPrototype *proto;
	TB_Symbol *symbol;

	Entity *  entity;
	cgModule *module;
	String    name;
	Type *    type;
	Ast *     type_expr;
	Ast *     body;
	u64       tags;
	ProcInlining inlining;
	bool         is_foreign;
	bool         is_export;
	bool         is_entry_point;
	bool         is_startup;

	TB_DebugType *debug_type;

	cgValue value;

	Ast *curr_stmt;

	cgTargetList *         target_list;
	Array<cgDefer>         defer_stack;
	Array<Scope *>         scope_stack;
	Array<cgContextData>   context_stack;

	Array<cgControlRegion> control_regions;
	Array<cgBranchRegions> branch_regions;

	Scope *curr_scope;
	i32    scope_index;
	bool   in_multi_assignment;
	isize  split_returns_index;
	bool   return_by_ptr;

	PtrMap<Entity *, cgAddr> variable_map;
	PtrMap<Entity *, cgAddr> soa_values_map;

	// NOTE(odin-tilde): deterministic per-proc counter for generated
	// global/proc names; a proc body is codegen'd sequentially, so this is
	// stable across runs (unlike a shared atomic counter).
	u32 generated_name_guid;

	// NOTE(odin-tilde): compile this proc with the peeps-only pipeline
	// regardless of TB_OPT_STAGE — set on the rebuilt procedure after an
	// RA bail so the retry cannot re-enter the pathological optimization
	// path (and a second bail aborts loudly instead of looping).
	bool force_peeps_only;
};


struct cgModule {
	TB_Module *  mod;
	Checker *    checker;
	CheckerInfo *info;
	LinkerData * linker_data;

	bool do_threading;
	Array<cgProcedure *> single_threaded_procedure_queue;

	RwMutex values_mutex;
	PtrMap<Entity *, cgValue>       values;
	PtrMap<Entity *, TB_Symbol *>   symbols;
	StringMap<cgValue>              members;
	StringMap<cgProcedure *>        procedures;
	PtrMap<TB_Function *, Entity *> procedure_values;

	// NOTE(odin-tilde): deterministic build support. The checker assigns
	// entity ids from a global atomic counter, so both id values and the
	// order of info->entities depend on thread scheduling (they race even
	// with -thread-count:1). These maps re-establish a deterministic order:
	// entity_ranks   — index of the entity in the fully sorted entity array
	//                  (used to sort codegen emission order)
	// entity_ordinals — index of the entity within its (pkg, name) group,
	//                  sorted by source position (used for the -N symbol
	//                  suffix instead of the racy entity id)
	PtrMap<Entity *, u64> entity_ranks;
	PtrMap<Entity *, u64> entity_ordinals;
	Array<Entity *>       entities_sorted;

	RecursiveMutex debug_type_mutex;
	PtrMap<Type *, TB_DebugType *> debug_type_map;
	PtrMap<Type *, TB_DebugType *> proc_debug_type_map; // not pointer to

	RecursiveMutex proc_proto_mutex;
	PtrMap<Type *, TB_FunctionPrototype *> proc_proto_map;

	BlockingMutex anonymous_proc_lits_mutex;
	PtrMap<Ast *, cgProcedure *> anonymous_proc_lits_map;

	RecursiveMutex generated_procs_mutex;
	PtrMap<Type *, cgProcedure *> equal_procs;
	PtrMap<Type *, cgProcedure *> hasher_procs;
	PtrMap<Type *, cgProcedure *> map_get_procs;
	PtrMap<Type *, cgProcedure *> map_set_procs;

	RecursiveMutex map_info_mutex;
	PtrMap<Type *, TB_Symbol *> map_info_map;
	PtrMap<Type *, TB_Symbol *> map_cell_info_map;

	// NOTE(odin-tilde): structurally identical types from different
	// instantiations are distinct Type* but share a type string, so
	// type-keyed dedup alone produces same-named twin symbols whose layout
	// order is not deterministic; dedup those by generated name as well.
	StringMap<TB_Symbol *>   generated_globals_by_name; // mapci/mapi
	StringMap<cgProcedure *> generated_procs_by_name;   // equal/hasher

	// NOTE(bill): no need to protect this with a mutex
	PtrMap<uintptr, TB_SourceFile *> file_id_map; // Key: AstFile.id (i32 cast to uintptr)

	std::atomic<u32> nested_type_name_guid;
	std::atomic<u32> const_nil_guid;

	// NOTE(odin-tilde): dummy global used as the target of LOCKed RMWs to
	// implement atomic fences (TB has no fence node).
	TB_Global *fence_dummy;

	// NOTE(odin-tilde): lazily created libc memmove extern + prototype used
	// by cg_builtin_mem_copy (intrinsics.mem_copy is overlap-safe in Odin,
	// but TB only has a forward memcpy node). Codegen is single-threaded
	// here, so lazy init needs no lock.
	TB_Symbol *            libc_memmove_symbol;
	TB_FunctionPrototype * libc_memmove_proto;

	// NOTE(odin-tilde): Cuik has no Mach-O TLV support, so @(thread_local)
	// variables are lowered to offsets into a per-thread table fetched via
	// pthread key (runtime __tilde_tls_base). Offsets are assigned lazily on
	// first materialization; the table has a fixed compile-time size
	// (CG_TLS_TABLE_SIZE) so late assignment cannot outgrow it.
	BlockingMutex           tls_mutex;
	PtrMap<Entity *, i64>   tls_offsets;
	i64                     tls_size = 0;
};

#ifndef ABI_PKG_NAME_SEPARATOR
#define ABI_PKG_NAME_SEPARATOR "@"
#endif

struct GlobalTypeInfoData {
	TB_Global *global;
	Type *     array_type;
	Type *     elem_type;
	isize      index;
};

gb_global Entity *cg_global_type_info_data_entity   = {};
gb_global GlobalTypeInfoData cg_global_type_info_member_types       = {};
gb_global GlobalTypeInfoData cg_global_type_info_member_names       = {};
gb_global GlobalTypeInfoData cg_global_type_info_member_offsets     = {};
gb_global GlobalTypeInfoData cg_global_type_info_member_usings      = {};
gb_global GlobalTypeInfoData cg_global_type_info_member_tags        = {};
gb_global GlobalTypeInfoData cg_global_type_info_member_enum_values = {};

gb_global cgProcedure *cg_startup_runtime_proc = nullptr;
gb_global cgProcedure *cg_cleanup_runtime_proc = nullptr;



gb_internal TB_Arena *cg_arena(void);

gb_internal cgProcedure *cg_procedure_create(cgModule *m, Entity *entity, bool ignore_body=false);
gb_internal void cg_add_procedure_to_queue(cgProcedure *p);
gb_internal void cg_add_foreign_library_path(cgModule *m, Entity *e);
gb_internal void cg_setup_type_info_data(cgModule *m);
gb_internal cgProcedure *cg_procedure_generate_anonymous(cgModule *m, Ast *expr, cgProcedure *parent);
gb_internal void cg_procedure_generate(cgProcedure *p);
gb_internal void cg_push_context_onto_stack_from_implicit_parameter(cgProcedure *p);

gb_internal isize cg_global_const_calculate_region_count(ExactValue const &value, Type *type);
gb_internal i64   cg_global_const_calculate_region_count_from_basic_type(Type *type);
gb_internal bool  cg_global_const_add_region(cgModule *m, ExactValue const &value, Type *type, TB_Global *global, i64 offset);

// Small aggregates (<= 8 bytes, e.g. [3]u8 RGB_Pixel) are classified
// TB_PASSING_DIRECT on System-V and travel in a single GPR as an integer
// blob. These helpers pick the transport dt, load the blob from memory
// without ever reading past `size`, and give the local capacity needed to
// store a transport-width blob for a `size`-byte aggregate.
gb_internal TB_DataType cg_gpr_blob_dt(i64 size);
gb_internal i64         cg_gpr_blob_capacity(i64 size);
gb_internal TB_Node *   cg_load_gpr_blob(cgProcedure *p, TB_Node *addr, i64 size, i64 align);

// ------------------------------------------------------------------
// System-V AMD64 small-aggregate classification for C calling-convention
// boundaries (tilde_abi.cpp). TB classifies a whole aggregate as a
// single GPR blob or memory, but SysV splits <= 16-byte composites into
// per-eightbyte INTEGER/SSE classes riding RAX:RDX / XMM0:XMM1. The
// C-cc prototype, call site, callee prologue and return emission all
// consult these helpers so both sides of a C boundary agree.
// ------------------------------------------------------------------
struct cgSysVClass {
	i64  n_chunks;   // 0 => MEMORY class (indirect pointer); 1..2 register chunks
	bool sse[2];     // per-chunk register class (SSE xmm vs INTEGER gpr)
	i64  size[2];    // per-chunk byte size (chunk 0 is 8 when n_chunks == 2)
};

gb_internal bool         cg_sysv_abi_active(void);
gb_internal cgSysVClass  cg_sysv_classify(Type *type);
gb_internal bool         cg_sysv_needs_split(Type *type);
gb_internal TB_DataType  cg_sysv_chunk_dt(cgSysVClass const &sc, int k);
gb_internal TB_DebugType *cg_sysv_chunk_debug_type(cgModule *m, cgSysVClass const &sc, int k);

gb_internal String cg_get_entity_name(cgModule *m, Entity *e);

gb_internal cgValue cg_value(TB_Global *  g,    Type *type);
gb_internal cgValue cg_value(TB_External *e,    Type *type);
gb_internal cgValue cg_value(TB_Function *f,    Type *type);
gb_internal cgValue cg_value(TB_Symbol *  s,    Type *type);
gb_internal cgValue cg_value(TB_Node *    node, Type *type);

gb_internal cgAddr cg_addr(cgValue const &value);
gb_internal cgAddr cg_addr_map(cgValue addr, cgValue map_key, Type *map_type, Type *map_result);
gb_internal cgAddr cg_addr_bit_field(cgValue addr, Type *type, i64 bit_offset, u8 bit_size);
gb_internal cgAddr cg_addr_swizzle(cgValue addr, Type *type, u8 swizzle_count, u8 swizzle_indices[4]);

gb_internal u64 cg_typeid_as_u64(cgModule *m, Type *type);
gb_internal cgValue cg_type_info(cgProcedure *p, Type *type);
gb_internal isize cg_type_info_index(CheckerInfo *info, Type *type, bool err_on_not_found=true);

gb_internal cgValue cg_const_value(cgProcedure *p, Type *type, ExactValue const &value);
gb_internal cgValue cg_const_nil(cgProcedure *p, Type *type);

gb_internal cgValue cg_flatten_value(cgProcedure *p, cgValue value);

gb_internal void cg_build_stmt(cgProcedure *p, Ast *stmt);
gb_internal void cg_build_stmt_list(cgProcedure *p, Slice<Ast *> const &stmts);
gb_internal void cg_build_when_stmt(cgProcedure *p, AstWhenStmt *ws);


gb_internal cgValue cg_build_expr(cgProcedure *p, Ast *expr);
gb_internal cgAddr  cg_build_addr(cgProcedure *p, Ast *expr);
gb_internal cgValue cg_build_addr_ptr(cgProcedure *p, Ast *expr);
gb_internal cgValue cg_build_cond(cgProcedure *p, Ast *cond, TB_Node *true_block, TB_Node *false_block);

gb_internal Type *  cg_addr_type(cgAddr const &addr);
gb_internal cgValue cg_addr_load(cgProcedure *p, cgAddr addr);
gb_internal void    cg_addr_store(cgProcedure *p, cgAddr addr, cgValue value);
gb_internal cgValue cg_addr_get_ptr(cgProcedure *p, cgAddr const &addr);

gb_internal cgValue cg_emit_load(cgProcedure *p, cgValue const &ptr, bool is_volatile=false);
gb_internal void    cg_emit_store(cgProcedure *p, cgValue dst, cgValue src, bool is_volatile=false);

gb_internal cgAddr  cg_add_local (cgProcedure *p, Type *type, Entity *e, bool zero_init);
gb_internal cgAddr  cg_add_global(cgProcedure *p, Type *type, Entity *e);
gb_internal cgValue cg_address_from_load_or_generate_local(cgProcedure *p, cgValue value);
gb_internal cgValue cg_copy_value_to_ptr(cgProcedure *p, cgValue value, Type *original_type, isize min_alignment);

gb_internal cgValue cg_build_call_expr(cgProcedure *p, Ast *expr);

gb_internal void cg_build_return_stmt(cgProcedure *p, Slice<Ast *> const &return_results);
gb_internal void cg_build_return_stmt_internal(cgProcedure *p, Slice<cgValue> const &results);
gb_internal void cg_build_return_stmt_internal_single(cgProcedure *p, cgValue result);
gb_internal void cg_build_range_stmt(cgProcedure *p, Ast *node);

gb_internal cgValue cg_find_value_from_entity(cgModule *m, Entity *e);
gb_internal cgValue cg_find_procedure_value_from_entity(cgModule *m, Entity *e);

gb_internal TB_DebugType *cg_debug_type(cgModule *m, Type *type);

gb_internal String cg_get_entity_name(cgModule *m, Entity *e);

gb_internal String cg_typed_generated_name(char const *prefix, Type *type);

gb_internal cgValue cg_typeid(cgProcedure *m, Type *t);

gb_internal cgValue cg_emit_ptr_offset(cgProcedure *p, cgValue ptr, cgValue index);
gb_internal cgValue cg_emit_array_ep(cgProcedure *p, cgValue s, cgValue index);
gb_internal cgValue cg_emit_array_epi(cgProcedure *p, cgValue s, i64 index);
gb_internal cgValue cg_emit_matrix_ep(cgProcedure *p, cgValue s, cgValue row, cgValue column);
gb_internal cgValue cg_emit_matrix_epi(cgProcedure *p, cgValue s, i64 row, i64 column);
gb_internal cgValue cg_emit_struct_ep(cgProcedure *p, cgValue s, i64 index);
gb_internal cgValue cg_emit_deep_field_gep(cgProcedure *p, cgValue e, Selection const &sel);
gb_internal cgValue cg_emit_struct_ev(cgProcedure *p, cgValue s, i64 index);

gb_internal cgValue cg_emit_conv(cgProcedure *p, cgValue value, Type *t);
gb_internal void    cg_emit_store_union_variant(cgProcedure *p, cgValue parent_ptr, cgValue value, Type *variant_type);
gb_internal cgValue cg_emit_union_tag_ptr(cgProcedure *p, cgValue const &parent_ptr);
gb_internal cgValue cg_const_union_tag(cgProcedure *p, Type *u, Type *v);
gb_internal cgValue cg_emit_comp_against_nil(cgProcedure *p, TokenKind op_kind, cgValue x);
gb_internal cgValue cg_emit_comp(cgProcedure *p, TokenKind op_kind, cgValue left, cgValue right);
gb_internal cgValue cg_emit_arith(cgProcedure *p, TokenKind op, cgValue lhs, cgValue rhs, Type *type);
gb_internal cgValue cg_emit_arith_matrix(cgProcedure *p, TokenKind op, cgValue lhs, cgValue rhs, Type *type, bool component_wise);
gb_internal cgValue cg_emit_unary_arith(cgProcedure *p, TokenKind op, cgValue x, Type *type);

// NOTE(odin-tilde): 128-bit integers (u128/i128 + endian variants) are opaque
// 16-byte aggregates; these helpers lower their ops to u64 limb pairs.
gb_internal bool     cg_is_type_integer_128(Type *t);
gb_internal bool     cg_u128_is_swapped(Type *t);
gb_internal TB_Node *cg_emit_u128_limb_load(cgProcedure *p, TB_Node *base_ptr, bool swapped, int limb);
gb_internal void     cg_emit_u128_limb_store(cgProcedure *p, TB_Node *base_ptr, bool swapped, int limb, TB_Node *n);
gb_internal TB_Node *cg_u128_addr_of(cgProcedure *p, cgValue v);
gb_internal cgValue  cg_u128_make_value(cgProcedure *p, Type *type, bool swapped, TB_Node *lo, TB_Node *hi);
gb_internal cgValue  cg_emit_arith_u128(cgProcedure *p, TokenKind op, cgValue lhs, cgValue rhs, Type *type);
gb_internal cgValue  cg_emit_arith_simd(cgProcedure *p, TokenKind op, cgValue lhs, cgValue rhs, Type *type);
gb_internal cgValue  cg_emit_comp_u128(cgProcedure *p, TokenKind op_kind, cgValue left, cgValue right);
gb_internal cgValue  cg_emit_conv_int128(cgProcedure *p, cgValue value, Type *src, Type *dst, Type *t);
gb_internal void    cg_emit_increment(cgProcedure *p, cgValue addr);

gb_internal cgProcedure *cg_equal_proc_for_type (cgModule *m, Type *type);
gb_internal cgProcedure *cg_hasher_proc_for_type(cgModule *m, Type *type);
gb_internal cgValue     cg_hasher_proc_value_for_type(cgProcedure *p, Type *type);
gb_internal cgValue     cg_equal_proc_value_for_type(cgProcedure *p, Type *type);

// NOTE(odin-tilde): pthread-key TLS shim (Cuik lacks Mach-O TLV). Fixed-size
// per-thread table; offsets are assigned lazily per TLS entity.
#define CG_TLS_TABLE_SIZE (1 << 20)
gb_internal bool    cg_entity_is_thread_local(Entity *e);
gb_internal i64     cg_tls_offset_for(cgModule *m, Entity *e);
gb_internal cgValue cg_tls_address(cgProcedure *p, Entity *e);
gb_internal void    cg_tls_ensure_runtime_globals(cgModule *m);
gb_internal bool    cg_exact_value_is_zero(ExactValue const &v);
// constant_value == nullptr 表示存在运行时（非常量）初始化表达式
gb_internal void    cg_check_thread_local_initializer(Entity *e, ExactValue const *constant_value);

gb_internal cgValue cg_emit_call(cgProcedure * p, cgValue value, Slice<cgValue> const &args);
gb_internal cgValue cg_emit_runtime_call(cgProcedure *p, char const *name, Slice<cgValue> const &args);

gb_internal bool    cg_emit_goto(cgProcedure *p, TB_Node *control_region);

gb_internal cgBranchRegions cg_lookup_branch_regions(cgProcedure *p, Ast *ident);
gb_internal void            cg_emit_defer_stmts(cgProcedure *p, cgDeferExitKind kind, TB_Node *control_region);
gb_internal void            cg_add_defer_proc(cgProcedure *p, isize scope_index, cgValue deferred, Slice<cgValue> const &result_as_args);

gb_internal TB_Node *cg_control_region(cgProcedure *p, char const *name);
gb_internal TB_Node *cg_control_region_loop(cgProcedure *p, char const *name);

gb_internal isize cg_append_tuple_values(cgProcedure *p, Array<cgValue> *dst_values, cgValue src_value);

gb_internal cgValue cg_handle_param_value(cgProcedure *p, Type *parameter_type, ParameterValue const &param_value, TokenPos const &pos);

gb_internal cgValue cg_builtin_len(cgProcedure *p, cgValue value);
gb_internal cgValue cg_builtin_raw_data(cgProcedure *p, cgValue const &x);
gb_internal cgValue cg_builtin_map_info(cgProcedure *p, Type *map_type);
gb_internal TB_Symbol *cg_map_info_symbol(cgModule *m, Type *map_type);
gb_internal cgValue cg_builtin_map_cell_info(cgProcedure *p, Type *type);
gb_internal cgValue cg_emit_source_code_location_as_global(cgProcedure *p, String const &proc_name, TokenPos pos);
gb_internal cgValue cg_emit_source_code_location_as_global(cgProcedure *p, Ast *node);


gb_internal cgValue cg_internal_dynamic_map_get_ptr(cgProcedure *p, cgValue const &map_ptr, cgValue const &key);
gb_internal cgValue cg_dynamic_map_reserve(cgProcedure *p, cgValue const &map_ptr, isize const capacity, Ast *node);
gb_internal void cg_internal_dynamic_map_set(cgProcedure *p, cgValue const &map_ptr, Type *map_type,
                                             cgValue const &map_key, cgValue const &map_value, Ast *node);