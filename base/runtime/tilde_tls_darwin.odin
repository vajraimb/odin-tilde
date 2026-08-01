#+build darwin
#+private
package runtime

import "base:intrinsics"

// TLS shim for the tilde (Cuik TB) backend.
//
// Cuik does not implement Mach-O TLV access sequences (its TLS support is
// Windows-centric), so the tilde emitter lowers every @(thread_local)
// variable access to an offset into a per-thread table fetched through a
// pthread key (this file). The LLVM backend emits real TLV and never calls
// __tilde_tls_base, but this file must still compile under it.
//
// Semantics: the table is zero-initialized on first use per thread and freed
// by the pthread key destructor at thread exit. Non-zero @(thread_local)
// initializers are rejected at compile time by the tilde emitter until
// template copying is implemented.

foreign import libc "system:System"

@(default_calling_convention="c")
foreign libc {
	@(link_name="pthread_key_create")  _tilde_tls_key_create  :: proc(key: ^u64, destructor: proc "c" (rawptr)) -> i32 ---
	@(link_name="pthread_getspecific") _tilde_tls_getspecific :: proc(key: u64) -> rawptr ---
	@(link_name="pthread_setspecific") _tilde_tls_setspecific :: proc(key: u64, value: rawptr) -> i32 ---
}

_tilde_tls_key:   u64
_tilde_tls_state: u32 // 0 = uninit, 1 = creating, 2 = ready

_tilde_tls_free :: proc "c" (p: rawptr) {
	if p != nil {
		_heap_free(p)
	}
}

// __tilde_tls_base returns the base of the current thread's zero-initialized
// TLS table of `size` bytes, allocating both the pthread key (once, globally)
// and the table (once per thread) lazily.
__tilde_tls_base :: proc "contextless" (size: int) -> rawptr {
	if intrinsics.atomic_load(&_tilde_tls_state) != 2 {
		_, claimed := intrinsics.atomic_compare_exchange_strong(&_tilde_tls_state, 0, 1)
		if claimed {
			_tilde_tls_key_create(&_tilde_tls_key, _tilde_tls_free)
			intrinsics.atomic_store(&_tilde_tls_state, 2)
		} else {
			for intrinsics.atomic_load(&_tilde_tls_state) != 2 {}
		}
	}
	p := _tilde_tls_getspecific(_tilde_tls_key)
	if p == nil {
		p = _heap_alloc(size) // zero-initialized (calloc)
		_tilde_tls_setspecific(_tilde_tls_key, p)
	}
	return p
}
