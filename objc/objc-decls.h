// $Id$

#ifndef __objc_decls_H__
#define __objc_decls_H__

#ifdef GNUSTEP_WITH_DLL

#if BUILD_libobjc_DLL
#
# if defined(__MINGW32__)
  /* On Mingw, the compiler will export all symbols automatically, so
   * __declspec(dllexport) is not needed.
   */
#  define objc_EXPORT  extern
#  define objc_DECLARE 
# else
#  define objc_EXPORT  __declspec(dllexport)
#  define objc_DECLARE __declspec(dllexport)
# endif
#else
#  define objc_EXPORT  extern __declspec(dllimport)
#  define objc_DECLARE extern __declspec(dllimport)
#endif

#else

#  define objc_EXPORT  extern
#  define objc_DECLARE 

#endif

#endif /* __objc_decls_H__ */
