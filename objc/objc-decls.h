// $Id$

#ifndef __objc_decls_H__
#define __objc_decls_H__

#if BUILD_libobjc_DLL
#  define objc_EXPORT  __declspec(dllexport)
#  define objc_DECLARE __declspec(dllexport)
#elif libobjc_ISDLL
#  define objc_EXPORT  extern __declspec(dllimport)
#  define objc_DECLARE extern __declspec(dllimport)
#else
#  define objc_EXPORT  extern
#  define objc_DECLARE 
#endif

#endif /* __objc_decls_H__ */
