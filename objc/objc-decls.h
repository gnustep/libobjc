// $Id$

#ifndef __objc_decls_H__
#define __objc_decls_H__

#ifdef GNUSTEP_WITH_DLL

#if BUILD_libobjc_DLL
#  define objc_EXPORT  __declspec(dllexport)
#  define objc_DECLARE __declspec(dllexport)
#else
#  define objc_EXPORT  extern __declspec(dllimport)
#  define objc_DECLARE extern __declspec(dllimport)
#endif

#else

#  define objc_EXPORT  extern
#  define objc_DECLARE 

#endif

#endif /* __objc_decls_H__ */
