/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#pragma once
#ifndef RP_COMMON_H
#define RP_COMMON_H

//-------------------------------------------------------------
// Headers and defines
//-------------------------------------------------------------

#include <sys/types.h>  // ssize_t
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "../include/repline.h"  // rp_malloc_fun_t, rp_color_t etc.

# ifdef __cplusplus
#  define rp_extern_c   extern "C"
# else
#  define rp_extern_c
# endif

#if defined(RP_SEPARATE_OBJS)
#  define rp_public     rp_extern_c 
# if defined(__GNUC__) // includes clang and icc      
#  define rp_private    __attribute__((visibility("hidden")))
# else
#  define rp_private  
# endif
#else
# define rp_private     static
# define rp_public      rp_extern_c
#endif

#define rp_unused(x)    (void)(x)


//-------------------------------------------------------------
// ssize_t
//-------------------------------------------------------------

#if defined(_MSC_VER)
typedef intptr_t ssize_t;
#endif

#define ssizeof(tp)   (ssize_t)(sizeof(tp))
static inline size_t  to_size_t(ssize_t sz) { return (sz >= 0 ? (size_t)sz : 0); }
static inline ssize_t to_ssize_t(size_t sz) { return (sz <= SIZE_MAX/2 ? (ssize_t)sz : 0); }

rp_private void    rp_memmove(void* dest, const void* src, ssize_t n);
rp_private void    rp_memcpy(void* dest, const void* src, ssize_t n);
rp_private void    rp_memset(void* dest, uint8_t value, ssize_t n);
rp_private bool    rp_memnmove(void* dest, ssize_t dest_size, const void* src, ssize_t n);

rp_private ssize_t rp_strlen(const char* s);
rp_private bool    rp_strcpy(char* dest, ssize_t dest_size /* including 0 */, const char* src);
rp_private bool    rp_strncpy(char* dest, ssize_t dest_size /* including 0 */, const char* src, ssize_t n);

rp_private bool    rp_contains(const char* big, const char* s);
rp_private bool    rp_icontains(const char* big, const char* s);
rp_private char    rp_tolower(char c);
rp_private int     rp_stricmp(const char* s1, const char* s2);



//---------------------------------------------------------------------
// Unicode
//
// We use "qutf-8" (quite like utf-8) encoding and decoding. 
// Internally we always use valid utf-8. If we encounter invalid
// utf-8 bytes (or bytes >= 0x80 from any other encoding) we encode
// these as special code points in the "raw plane" (0xEE000 - 0xEE0FF).
// When decoding we are then able to restore such raw bytes as-is.
// See <https://github.com/koka-lang/koka/blob/master/kklib/include/kklib/string.h>
//---------------------------------------------------------------------

typedef uint32_t  unicode_t;

rp_private void      unicode_to_qutf8(unicode_t u, uint8_t buf[5]);
rp_private unicode_t unicode_from_qutf8(const uint8_t* s, ssize_t len, ssize_t* nread); // validating

rp_private unicode_t unicode_from_raw(uint8_t c);
rp_private bool      unicode_is_raw(unicode_t u, uint8_t* c);

rp_private bool      utf8_is_cont(uint8_t c);

//-------------------------------------------------------------
// Debug
//-------------------------------------------------------------

#if defined(RP_NO_DEBUG_MSG) 
#define debug_msg(fmt,...)   (void)(0)
#else
rp_private void debug_msg( const char* fmt, ... );
#endif


//-------------------------------------------------------------
// Abstract environment
//-------------------------------------------------------------
struct rp_env_s;
typedef struct rp_env_s rp_env_t;


//-------------------------------------------------------------
// Allocation
//-------------------------------------------------------------

typedef struct alloc_s {
  rp_malloc_fun_t*  malloc;
  rp_realloc_fun_t* realloc;
  rp_free_fun_t*    free;
} alloc_t;


rp_private void* mem_malloc( alloc_t* mem, ssize_t sz );
rp_private void* mem_zalloc( alloc_t* mem, ssize_t sz );
rp_private void* mem_realloc( alloc_t* mem, void* p, ssize_t newsz );
rp_private void  mem_free( alloc_t* mem, const void* p );
rp_private char* mem_strdup( alloc_t* mem, const char* s);
rp_private char* mem_strndup( alloc_t* mem, const char* s, ssize_t n);

#define mem_zalloc_tp(mem,tp)        (tp*)mem_zalloc(mem,ssizeof(tp))
#define mem_malloc_tp_n(mem,tp,n)    (tp*)mem_malloc(mem,(n)*ssizeof(tp))
#define mem_zalloc_tp_n(mem,tp,n)    (tp*)mem_zalloc(mem,(n)*ssizeof(tp))
#define mem_realloc_tp(mem,tp,p,n)   (tp*)mem_realloc(mem,p,(n)*ssizeof(tp))


#endif // RP_COMMON_H
