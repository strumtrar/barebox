/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_COMPILER_TYPES_H
#define __LINUX_COMPILER_TYPES_H

#ifndef __ASSEMBLY__

#ifdef __CHECKER__
# define __user		__attribute__((noderef, address_space(1)))
# define __kernel	__attribute__((address_space(0)))
# define __safe		__attribute__((safe))
# define __force	__attribute__((force))
# define __nocast	__attribute__((nocast))
# define __iomem	__attribute__((noderef, address_space(2)))
# define __must_hold(x)	__attribute__((context(x,1,1)))
# define __acquires(x)	__attribute__((context(x,0,1)))
# define __releases(x)	__attribute__((context(x,1,0)))
# define __acquire(x)	__context__(x,1)
# define __release(x)	__context__(x,-1)
# define __cond_lock(x,c)	((c) ? ({ __acquire(x); 1; }) : 0)
# define __percpu	__attribute__((noderef, address_space(3)))
# define __rcu		__attribute__((noderef, address_space(4)))
# define __private	__attribute__((noderef))
extern void __chk_user_ptr(const volatile void __user *);
extern void __chk_io_ptr(const volatile void __iomem *);
# define ACCESS_PRIVATE(p, member) (*((typeof((p)->member) __force *) &(p)->member))
#else /* __CHECKER__ */
# ifdef STRUCTLEAK_PLUGIN
#  define __user __attribute__((user))
# else
#  define __user
# endif
# define __kernel
# define __safe
# define __force
# define __nocast
# define __iomem
# define __chk_user_ptr(x) (void)0
# define __chk_io_ptr(x) (void)0
# define __builtin_warning(x, y...) (1)
# define __must_hold(x)
# define __acquires(x)
# define __releases(x)
# define __acquire(x) (void)0
# define __release(x) (void)0
# define __cond_lock(x,c) (c)
# define __percpu
# define __rcu
# define __private
# define ACCESS_PRIVATE(p, member) ((p)->member)
#endif /* __CHECKER__ */

/* Indirect macros required for expanded argument pasting, eg. __LINE__. */
#define ___PASTE(a,b) a##b
#define __PASTE(a,b) ___PASTE(a,b)

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-cleanup-variable-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#cleanup
 */
#define __cleanup(func)			__attribute__((__cleanup__(func)))

#ifdef __KERNEL__

/*
 * Note: do not use this directly. Instead, use __alloc_size() since it is conditionally
 * available and includes other attributes. For GCC < 9.1, __alloc_size__ gets undefined
 * in compiler-gcc.h, due to misbehaviors.
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-alloc_005fsize-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#alloc-size
 */
#define __alloc_size__(x, ...)		__attribute__((__alloc_size__(x, ## __VA_ARGS__)))

/* Compiler specific macros. */
#ifdef __clang__
#include <linux/compiler-clang.h>
#elif defined(__GNUC__)
/* The above compilers also define __GNUC__, so order is important here. */
#include <linux/compiler-gcc.h>
#else
#error "Unknown compiler"
#endif

/*
 * Some architectures need to provide custom definitions of macros provided
 * by linux/compiler-*.h, and can do so using asm/compiler.h. We include that
 * conditionally rather than using an asm-generic wrapper in order to avoid
 * build failures if any C compilation, which will include this file via an
 * -include argument in c_flags, occurs prior to the asm-generic wrappers being
 * generated.
 */
#ifdef CONFIG_HAVE_ARCH_COMPILER_H
#include <asm/compiler.h>
#endif

/*
 * Generic compiler-independent macros required for kernel
 * build go below this comment. Actual compiler/compiler version
 * specific implementations come from the above header files
 */

struct ftrace_branch_data {
	const char *func;
	const char *file;
	unsigned line;
	union {
		struct {
			unsigned long correct;
			unsigned long incorrect;
		};
		struct {
			unsigned long miss;
			unsigned long hit;
		};
		unsigned long miss_hit[2];
	};
};

struct ftrace_likely_data {
	struct ftrace_branch_data	data;
	unsigned long			constant;
};

/* Don't. Just don't. */
#define __deprecated
#define __deprecated_for_modules

#ifndef __has_attribute
#define __has_attribute(...)		0
#endif

/*
 * Note: the "type" argument should match any __builtin_object_size(p, type) usage.
 *
 * Optional: not supported by gcc.
 *
 * clang: https://clang.llvm.org/docs/AttributeReference.html#pass-object-size-pass-dynamic-object-size
 */
#if __has_attribute(__pass_dynamic_object_size__)
# define __pass_dynamic_object_size(type)	__attribute__((__pass_dynamic_object_size__(type)))
#else
# define __pass_dynamic_object_size(type)
#endif
#if __has_attribute(__pass_object_size__)
# define __pass_object_size(type)	__attribute__((__pass_object_size__(type)))
#else
# define __pass_object_size(type)
#endif

/*
 * Add the pseudo keyword 'fallthrough' so case statement blocks
 * must end with any of these keywords:
 *   break;
 *   fallthrough;
 *   continue;
 *   goto <label>;
 *   return [expression];
 *
 *  gcc: https://gcc.gnu.org/onlinedocs/gcc/Statement-Attributes.html#Statement-Attributes
 */
#if __has_attribute(__fallthrough__)
# define fallthrough                    __attribute__((__fallthrough__))
#else
# define fallthrough                    do {} while (0)  /* fallthrough */
#endif

/*
 * Optional: only supported since GCC >= 11.1, clang >= 7.0.
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-no_005fstack_005fprotector-function-attribute
 *   clang: https://clang.llvm.org/docs/AttributeReference.html#no-stack-protector-safebuffers
 */
#if __has_attribute(__no_stack_protector__)
# define __no_stack_protector		__attribute__((__no_stack_protector__))
#elif ! defined CONFIG_STACKPROTECTOR
# define __no_stack_protector		__attribute__((__optimize__("-fno-stack-protector")))
#else
# define __no_stack_protector
#endif

#if __has_attribute(__returns_nonnull__)
#define __returns_nonnull		__attribute__((__returns_nonnull__))
#else
#define __returns_nonnull
#endif

/*
 * Optional: not supported by gcc.
 *
 * clang: https://clang.llvm.org/docs/AttributeReference.html#overloadable
 */
#if __has_attribute(__overloadable__)
# define __overloadable			__attribute__((__overloadable__))
#else
# define __overloadable
#endif

/*
 * Optional: not supported by gcc
 * Optional: only supported since clang >= 14.0
 *
 * clang: https://clang.llvm.org/docs/AttributeReference.html#diagnose_as_builtin
 */
#if __has_attribute(__diagnose_as_builtin__)
# define __diagnose_as(builtin...)	__attribute__((__diagnose_as_builtin__(builtin)))
#else
# define __diagnose_as(builtin...)
#endif

#endif /* __KERNEL__ */

#endif /* __ASSEMBLY__ */

/*
 * The below symbols may be defined for one or more, but not ALL, of the above
 * compilers. We don't consider that to be an error, so set them to nothing.
 * For example, some of them are for compiler specific plugins.
 */
#ifndef __designated_init
# define __designated_init
#endif

#ifndef __latent_entropy
# define __latent_entropy
#endif

#ifndef __randomize_layout
# define __randomize_layout __designated_init
#endif

#ifndef __no_randomize_layout
# define __no_randomize_layout
#endif

#ifndef randomized_struct_fields_start
# define randomized_struct_fields_start
# define randomized_struct_fields_end
#endif

#ifndef __visible
#define __visible
#endif

/*
 * Assume alignment of return value.
 */
#ifndef __assume_aligned
#define __assume_aligned(a, ...)
#endif

/*
 * Any place that could be marked with the "alloc_size" attribute is also
 * a place to be marked with the "malloc" attribute, except those that may
 * be performing a _reallocation_, as that may alias the existing pointer.
 * For these, use __realloc_size().
 */
#ifdef __alloc_size__
# define __alloc_size(x, ...)	__alloc_size__(x, ## __VA_ARGS__) __malloc
# define __realloc_size(x, ...)	__alloc_size__(x, ## __VA_ARGS__)
#else
# define __alloc_size(x, ...)	__malloc
# define __realloc_size(x, ...)
#endif

# define __xalloc_size(args...)		__returns_nonnull __alloc_size(args)
# define __xrealloc_size(args...)	__returns_nonnull __realloc_size(args)

/* Are two types/vars the same type (ignoring qualifiers)? */
#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))

/*
 * __unqual_scalar_typeof(x) - Declare an unqualified scalar type, leaving
 *			       non-scalar types unchanged.
 */
/*
 * Prefer C11 _Generic for better compile-times and simpler code. Note: 'char'
 * is not type-compatible with 'signed char', and we define a separate case.
 */
#define __scalar_type_to_expr_cases(type)				\
		unsigned type:	(unsigned type)0,			\
		signed type:	(signed type)0

#define __unqual_scalar_typeof(x) typeof(				\
		_Generic((x),						\
			 char:	(char)0,				\
			 __scalar_type_to_expr_cases(char),		\
			 __scalar_type_to_expr_cases(short),		\
			 __scalar_type_to_expr_cases(int),		\
			 __scalar_type_to_expr_cases(long),		\
			 __scalar_type_to_expr_cases(long long),	\
			 default: (x)))

/* Is this type a native word size -- useful for atomic operations */
#define __native_word(t) \
	(sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

#ifndef __attribute_const__
#define __attribute_const__	__attribute__((__const__))
#endif

#ifndef __noclone
#define __noclone
#endif

/* Helpers for emitting diagnostics in pragmas. */
#ifndef __diag
#define __diag(string)
#endif

#ifndef __diag_GCC
#define __diag_GCC(version, severity, string)
#endif

#define __diag_push()	__diag(push)
#define __diag_pop()	__diag(pop)

#define __diag_ignore(compiler, version, option, comment) \
	__diag_ ## compiler(version, ignore, option)
#define __diag_warn(compiler, version, option, comment) \
	__diag_ ## compiler(version, warn, option)
#define __diag_error(compiler, version, option, comment) \
	__diag_ ## compiler(version, error, option)

/*
 * From the GCC manual:
 *
 * Many functions have no effects except the return value and their
 * return value depends only on the parameters and/or global
 * variables.  Such a function can be subject to common subexpression
 * elimination and loop optimization just as an arithmetic operator
 * would be.
 * [...]
 */
#define __pure			__attribute__((pure))
#define __aligned(x)		__attribute__((aligned(x)))
#define __aligned_largest	__attribute__((aligned))
#define __printf(a, b)		__attribute__((format(__printf__, a, b)))
#define __scanf(a, b)		__attribute__((format(__scanf__, a, b)))
#define __maybe_unused		__attribute__((unused))
#define __always_unused		__attribute__((unused))
#define __mode(x)		__attribute__((mode(x)))
#define __malloc		__attribute__((__malloc__))
#define __used			__attribute__((__used__))
#define __noreturn		__attribute__((noreturn))
#define __packed		__attribute__((packed))
#define __weak			__attribute__((weak))
#define __alias(symbol)		__attribute__((alias(#symbol)))
#define __cold			__attribute__((cold))
#define __section(S)		__attribute__((__section__(#S)))

#ifdef __clang__
#define __ll_elem(S)		__section(S) __used __no_sanitize_address
#else
#define __ll_elem(S)		__section(S) __used
#endif


#ifdef CONFIG_ENABLE_MUST_CHECK
#define __must_check		__attribute__((warn_unused_result))
#else
#define __must_check
#endif

#if defined(CC_USING_HOTPATCH) && !defined(__CHECKER__)
#define notrace			__attribute__((hotpatch(0, 0)))
#else
#define notrace			__attribute__((no_instrument_function))
#endif

/*
 * it doesn't make sense on ARM (currently the only user of __naked)
 * to trace naked functions because then mcount is called without
 * stack and frame pointer being set up and there is no chance to
 * restore the lr register to the value before mcount was called.
 */
#define __naked			__attribute__((naked)) notrace

#define __compiler_offsetof(a, b)	__builtin_offsetof(a, b)

/*
 * Feature detection for gnu_inline (gnu89 extern inline semantics). Either
 * __GNUC_STDC_INLINE__ is defined (not using gnu89 extern inline semantics,
 * and we opt in to the gnu89 semantics), or __GNUC_STDC_INLINE__ is not
 * defined so the gnu89 semantics are the default.
 */
#ifdef __GNUC_STDC_INLINE__
# define __gnu_inline	__attribute__((gnu_inline))
#else
# define __gnu_inline
#endif

/*
 * Force always-inline if the user requests it so via the .config.
 * GCC does not warn about unused static inline functions for
 * -Wunused-function.  This turns out to avoid the need for complex #ifdef
 * directives.  Suppress the warning in clang as well by using "unused"
 * function attribute, which is redundant but not harmful for gcc.
 * Prefer gnu_inline, so that extern inline functions do not emit an
 * externally visible function. This makes extern inline behave as per gnu89
 * semantics rather than c99. This prevents multiple symbol definition errors
 * of extern inline functions at link time.
 * A lot of inline functions can cause havoc with function tracing.
 */
#if !defined(CONFIG_ARCH_SUPPORTS_OPTIMIZED_INLINING) || \
	!defined(CONFIG_OPTIMIZE_INLINING)
#define inline \
	inline __attribute__((always_inline, unused)) notrace __gnu_inline
#else
#define inline inline	__attribute__((unused)) notrace __gnu_inline
#endif

#define __inline__ inline
#define __inline inline
#define noinline	__attribute__((noinline))

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

/*
 * Rather then using noinline to prevent stack consumption, use
 * noinline_for_stack instead.  For documentation reasons.
 */
#define noinline_for_stack noinline

/*
 * Sanitizer helper attributes: Because using __always_inline and
 * __no_sanitize_* conflict, provide helper attributes that will either expand
 * to __no_sanitize_* in compilation units where instrumentation is enabled
 * (__SANITIZE_*__), or __always_inline in compilation units without
 * instrumentation (__SANITIZE_*__ undefined).
 */
#ifdef __SANITIZE_ADDRESS__
/*
 * We can't declare function 'inline' because __no_sanitize_address conflicts
 * with inlining. Attempt to inline it may cause a build failure.
 *     https://gcc.gnu.org/bugzilla/show_bug.cgi?id=67368
 * '__maybe_unused' allows us to avoid defined-but-not-used warnings.
 */
# define __no_kasan_or_inline __no_sanitize_address notrace __maybe_unused
# define __no_sanitize_or_inline __no_kasan_or_inline
#else
# define __no_kasan_or_inline __always_inline
#endif

#ifndef __no_sanitize_or_inline
#define __no_sanitize_or_inline __always_inline
#endif

/* code that can't be instrumented at all */
#define noinstr \
	noinline notrace __no_sanitize_address __no_stack_protector

#define __prereloc \
	notrace __no_sanitize_address __no_stack_protector

#endif /* __LINUX_COMPILER_TYPES_H */
