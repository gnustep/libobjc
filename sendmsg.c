/* GNU Objective C Runtime message lookup 
   Copyright (C) 1993, 1995, 1996, 1997, 1998,
   2001, 2002, 2004 Free Software Foundation, Inc.
   Contributed by Kresten Krab Thorup

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2, or (at your option) any later version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
details.

You should have received a copy of the GNU General Public License along with
GCC; see the file COPYING.  If not, write to the Free Software
Foundation, 51 Franklin Street, Fifth Floor,
Boston, MA 02110-1301, USA.  */

/* As a special exception, if you link this library with files compiled with
   GCC to produce an executable, this does not cause the resulting executable
   to be covered by the GNU General Public License. This exception does not
   however invalidate any other reasons why the executable file might be
   covered by the GNU General Public License.  */

/* FIXME: This file has no business including tm.h.  */
/* FIXME: This should be using libffi instead of __builtin_apply
   and friends.  */

//#include "tconfig.h"
//#include "coretypes.h"
//#include "tm.h"
#include "objc/runtime.h"
#include "objc/sarray.h"
#include "objc/encoding.h"
#include "runtime-info.h"

/* This is how we hack STRUCT_VALUE to be 1 or 0.   */
#define gen_rtx(args...) 1
#define gen_rtx_MEM(args...) 1
#define gen_rtx_REG(args...) 1
#define rtx int

#if ! defined (STRUCT_VALUE) || STRUCT_VALUE == 0
#define INVISIBLE_STRUCT_RETURN 1
#else
#define INVISIBLE_STRUCT_RETURN 0
#endif

/* The uninstalled dispatch table */
struct sarray *__objc_uninstalled_dtable = 0;   /* !T:MUTEX */

/* Two hooks for method forwarding. If either is set, it is invoked
 * to return a function that performs the real forwarding.  If both
 * are set, the result of __objc_msg_forward2 will be preferred over
 * that of __objc_msg_forward.  If both return NULL or are unset,
 * the libgcc based functions (__builtin_apply and friends) are
 * used.
 */
IMP (*__objc_msg_forward) (SEL) = NULL;
IMP (*__objc_msg_forward2) (id, SEL) = NULL;

/* Send +initialize to class */
static void __objc_send_initialize (Class);

/* Forward declare some functions */
static void __objc_install_dtable_for_class (Class cls);
static void __objc_prepare_dtable_for_class (Class cls);
static void __objc_install_prepared_dtable_for_class (Class cls);

static struct sarray *__objc_prepared_dtable_for_class (Class cls);
static IMP __objc_get_prepared_imp (Class cls,SEL sel);

/* Various forwarding functions that are used based upon the
   return type for the selector.
   __objc_block_forward for structures.
   __objc_double_forward for floats/doubles.
   __objc_word_forward for pointers or types that fit in registers. */
static double __objc_double_forward (id, SEL, ...);
static id __objc_word_forward (id, SEL, ...);
typedef struct { id many[8]; } __big;
#if INVISIBLE_STRUCT_RETURN 
static __big 
#else
static id
#endif
__objc_block_forward (id, SEL, ...);
static Method_t search_for_method_in_hierarchy (Class class, SEL sel);
Method_t search_for_method_in_list (MethodList_t list, SEL op);
id nil_method (id, SEL);

/* Given a selector, return the proper forwarding implementation. */
inline
IMP
__objc_get_forward_imp (id rcv, SEL sel)
{
  /* If a custom forwarding hook was registered, try getting a forwarding
     function from it. There are two forward routine hooks, one that
     takes the receiver as an argument and one that does not. */
  if (__objc_msg_forward2)
    {
      IMP result;
      if ((result = __objc_msg_forward2 (rcv, sel)) != NULL)
       return result;
    }
  if (__objc_msg_forward)
    {
      IMP result;
      if ((result = __objc_msg_forward (sel)) != NULL) 
        return result;
    }

  /* In all other cases, use the default forwarding functions built using
     __builtin_apply and friends.  */
    {
      const char *t = sel->sel_types;

      if (t && (*t == '[' || *t == '(' || *t == '{')
#ifdef OBJC_MAX_STRUCT_BY_VALUE
          && objc_sizeof_type (t) > OBJC_MAX_STRUCT_BY_VALUE
#endif
          )
        return (IMP)__objc_block_forward;
      else if (t && (*t == 'f' || *t == 'd'))
        return (IMP)__objc_double_forward;
      else
        return (IMP)__objc_word_forward;
    }
}

/* Given a CLASS and selector, return the implementation corresponding
   to the method of the selector.

   If CLASS is a class, the instance method is returned.
   If CLASS is a meta class, the class method is returned.

   Since this requires the dispatch table to be installed, this function
   will implicitly invoke +initialize for CLASS if it hasn't been
   invoked yet.  This also insures that +initialize has been invoked
   when the returned implementation is called directly.  */
inline
IMP
get_imp (Class class, SEL sel)
{
  /* In a vanilla implementation we would first check if the dispatch
     table is installed.  Here instead, to get more speed in the
     standard case (that the dispatch table is installed) we first try
     to get the imp using brute force.  Only if that fails, we do what
     we should have been doing from the very beginning, that is, check
     if the dispatch table needs to be installed, install it if it's
     not installed, and retrieve the imp from the table if it's
     installed.  */
  void *res = sarray_get_safe (class->dtable, (size_t) sel->sel_id);
  if (res == 0)
    {
      /* Not a valid method */
      if (class->dtable == __objc_uninstalled_dtable)
	{
	  /* The dispatch table needs to be installed. */
	  objc_mutex_lock (__objc_runtime_mutex);

	   /* Double-checked locking pattern: Check
	      __objc_uninstalled_dtable again in case another thread
	      installed the dtable while we were waiting for the lock
	      to be released.  */
         if (class->dtable == __objc_uninstalled_dtable)
	   __objc_install_dtable_for_class (class);

	 /* If the dispatch table is not yet installed,
	    we are still in the process of executing +initialize.
	    But the implementation pointer should be avaliable
	    if it exists at all.  */
         if (class->dtable == __objc_uninstalled_dtable)
	   {
	     assert (__objc_prepared_dtable_for_class (class) != 0);
	     res = __objc_get_prepared_imp (class, sel);
	   }

	  objc_mutex_unlock (__objc_runtime_mutex);
	  /* Call ourselves with the installed dispatch table
	     and get the real method */
	  if (! res)
	    res = get_imp (class, sel);
	}
      else
	{
	  /* The dispatch table has been installed.  */

         /* Get the method from the dispatch table (we try to get it
	    again in case another thread has installed the dtable just
	    after we invoked sarray_get_safe, but before we checked
	    class->dtable == __objc_uninstalled_dtable).
         */
	  res = sarray_get_safe (class->dtable, (size_t) sel->sel_id);
	  if (res == 0)
	    {
	      /* The dispatch table has been installed, and the method
		 is not in the dispatch table.  So the method just
		 doesn't exist for the class.  Return the forwarding
		 implementation.
		 We don't know the receiver (only it's class), so we
		 can't pass that to the function :-(
	       */
             res = __objc_get_forward_imp (nil, sel);
	    }
	}
    }
  return res;
}

/* Query if an object can respond to a selector, returns YES if the
   object implements the selector otherwise NO.  Does not check if the
   method can be forwarded. 
   Since this requires the dispatch table to installed, this function
   will implicitly invoke +initialize for the class of OBJECT if it
   hasn't been invoked yet.  */
inline
BOOL
__objc_responds_to (id object, SEL sel)
{
  void *res;
  struct sarray *dtable;

  /* Install dispatch table if need be */
  dtable = object->class_pointer->dtable;
  if (dtable == __objc_uninstalled_dtable)
    {
      objc_mutex_lock (__objc_runtime_mutex);
      if (object->class_pointer->dtable == __objc_uninstalled_dtable)
	__objc_install_dtable_for_class (object->class_pointer);
      
      /* If the dispatch table is not yet installed,
	 we are still in the process of executing +initialize.
	 Yet the dispatch table should be available.  */
      if (object->class_pointer->dtable == __objc_uninstalled_dtable)
	{
	  dtable = __objc_prepared_dtable_for_class (object->class_pointer);
	  assert (dtable);
	}
      else
	dtable = object->class_pointer->dtable;
      
      objc_mutex_unlock (__objc_runtime_mutex);
    }

  /* Get the method from the dispatch table */
  res = sarray_get_safe (dtable, (size_t) sel->sel_id);
  return (res != 0);
}

/* This is the lookup function.  All entries in the table are either a 
   valid method *or* zero.  If zero then either the dispatch table
   needs to be installed or it doesn't exist and forwarding is attempted. */
inline
IMP
objc_msg_lookup (id receiver, SEL op)
{
  IMP result;
  if (receiver)
    {
      result = sarray_get_safe (receiver->class_pointer->dtable, 
				(sidx)op->sel_id);
      if (result == 0)
	{
	  /* Not a valid method */
	  if (receiver->class_pointer->dtable == __objc_uninstalled_dtable)
	    {	      
	      objc_mutex_lock (__objc_runtime_mutex);
	      
	      if (receiver->class_pointer->dtable == __objc_uninstalled_dtable)
		{
		  /* The dispatch table needs to be installed.
		     This happens on the very first method call to the
		     class. */
		  __objc_install_dtable_for_class (receiver->class_pointer);

		  /* If we can get the implementation from the newly installed
		     dtable, +initialize has completed.  */
		  result = get_imp (receiver->class_pointer, op);

		  /* If not, messages are being sent during +initialize and
		     we should dispatch them without the installed dtable.
		     In this case we have locked the mutex recursively
		     so execution remains in this thread after our unlock. */
		  if (result == 0 
		      && (receiver->class_pointer->dtable 
			  == __objc_uninstalled_dtable))
		    result
		      = __objc_get_prepared_imp (receiver->class_pointer, op);
		}

	      objc_mutex_unlock (__objc_runtime_mutex);
	    }
	  else
	    {
	      /* The dispatch table has been installed.  Check again
		 if the method exists (just in case the dispatch table
		 has been installed by another thread after we did the
		 previous check that the method exists).
	      */
	      result = sarray_get_safe (receiver->class_pointer->dtable,
					(sidx)op->sel_id);
	      if (result == 0)
		{
		  /* If the method still just doesn't exist for the
		     class, attempt to forward the method. */
		  result = __objc_get_forward_imp (receiver, op);
		}
	    }
	}
      return result;
    }
  else
    return (IMP)nil_method;
}

IMP
objc_msg_lookup_super (Super_t super, SEL sel)
{
  if (super->self)
    return get_imp (super->class, sel);
  else
    return (IMP)nil_method;
}

int method_get_sizeof_arguments (Method *);

retval_t
objc_msg_sendv (id object, SEL op, arglist_t arg_frame)
{
  Method *m = class_get_instance_method (object->class_pointer, op);
  const char *type;
  *((id *) method_get_first_argument (m, arg_frame, &type)) = object;
  *((SEL *) method_get_next_argument (arg_frame, &type)) = op;
  return __builtin_apply ((apply_t) m->method_imp, 
			  arg_frame,
			  method_get_sizeof_arguments (m));
}

void
__objc_init_dispatch_tables ()
{
  __objc_uninstalled_dtable = sarray_new (200, 0);
}

/* Install dummy table for class which causes the first message to
   that class (or instances hereof) to be initialized properly */
void
__objc_install_premature_dtable (Class class)
{
  assert (__objc_uninstalled_dtable);
  class->dtable = __objc_uninstalled_dtable;
}   

/* Send +initialize to class if not already done */
static void
__objc_send_initialize (Class class)
{
  /* This *must* be a class object */
  assert (CLS_ISCLASS (class));
  assert (! CLS_ISMETA (class));

  /* class_add_method_list/__objc_update_dispatch_table_for_class
     may have reset the dispatch table.  The canonical way to insure
     that we send +initialize just once, is this flag.  */
  if (! CLS_ISINITIALIZED (class))
    {
      CLS_SETINITIALIZED (class);
      CLS_SETINITIALIZED (class->class_pointer);

      /* Create the garbage collector type memory description */
      __objc_generate_gc_type_description (class);

      if (class->super_class)
	__objc_send_initialize (class->super_class);

      {
	SEL 	     op = sel_register_name ("initialize");
	IMP	     imp = 0;
        MethodList_t method_list = class->class_pointer->methods;

        while (method_list) {
	  int i;
          Method_t method;

          for (i = 0; i < method_list->method_count; i++) {
	    method = &(method_list->method_list[i]);
            if (method->method_name
                && method->method_name->sel_id == op->sel_id) {
	      imp = method->method_imp;
              break;
            }
          }

          if (imp)
            break;

          method_list = method_list->method_next;

	}
	if (imp)
	    (*imp) ((id) class, op);
		
      }
    }
}

/* Walk on the methods list and install the methods in the reverse
   order of the lists. Since methods added by categories are before the methods
   of class in the methods list, this allows categories to substitute methods
   declared in class. However if more than one category replaces the same
   method nothing is guaranteed about what method will be used.
   Assumes that __objc_runtime_mutex is locked down. */
static void
__objc_install_methods_in_dtable (struct sarray *dtable, MethodList_t method_list)
{
  int i;

  if (! method_list)
    return;

  if (method_list->method_next)
    __objc_install_methods_in_dtable (dtable, method_list->method_next);

  for (i = 0; i < method_list->method_count; i++)
    {
      Method_t method = &(method_list->method_list[i]);
      sarray_at_put_safe (dtable,
			  (sidx) method->method_name->sel_id,
			  method->method_imp);
    }
}

void
__objc_update_dispatch_table_for_class (Class class)
{
  Class next;
  struct sarray *arr;

  objc_mutex_lock (__objc_runtime_mutex);

  /* not yet installed -- skip it unless in +initialize */
  if (class->dtable == __objc_uninstalled_dtable) 
    {
      if (__objc_prepared_dtable_for_class (class))
	{
	  /* There is a prepared table so we must be initialising this
	     class ... we must re-do the table preparation. */
	  __objc_prepare_dtable_for_class (class);
	}
      objc_mutex_unlock (__objc_runtime_mutex);
      return;
    }

  arr = class->dtable;
  __objc_install_premature_dtable (class); /* someone might require it... */
  sarray_free (arr);			   /* release memory */

  /* could have been lazy... */
  __objc_install_dtable_for_class (class); 

  if (class->subclass_list)	/* Traverse subclasses */
    for (next = class->subclass_list; next; next = next->sibling_class)
      __objc_update_dispatch_table_for_class (next);

  objc_mutex_unlock (__objc_runtime_mutex);
}


/* This function adds a method list to a class.  This function is
   typically called by another function specific to the run-time.  As
   such this function does not worry about thread safe issues.

   This one is only called for categories. Class objects have their
   methods installed right away, and their selectors are made into
   SEL's by the function __objc_register_selectors_from_class. */
void
class_add_method_list (Class class, MethodList_t list)
{
  /* Passing of a linked list is not allowed.  Do multiple calls.  */
  assert (! list->method_next);

  __objc_register_selectors_from_list(list);

  /* Add the methods to the class's method list.  */
  list->method_next = class->methods;
  class->methods = list;

  /* Update the dispatch table of class */
  __objc_update_dispatch_table_for_class (class);
}

Method_t
class_get_instance_method (Class class, SEL op)
{
  return search_for_method_in_hierarchy (class, op);
}

Method_t
class_get_class_method (MetaClass class, SEL op)
{
  return search_for_method_in_hierarchy (class, op);
}


/* Search for a method starting from the current class up its hierarchy.
   Return a pointer to the method's method structure if found.  NULL
   otherwise. */   

static Method_t
search_for_method_in_hierarchy (Class cls, SEL sel)
{
  Method_t method = NULL;
  Class class;

  if (! sel_is_mapped (sel))
    return NULL;

  /* Scan the method list of the class.  If the method isn't found in the
     list then step to its super class. */
  for (class = cls; ((! method) && class); class = class->super_class)
    method = search_for_method_in_list (class->methods, sel);

  return method;
}



/* Given a linked list of method and a method's name.  Search for the named
   method's method structure.  Return a pointer to the method's method
   structure if found.  NULL otherwise. */  
Method_t
search_for_method_in_list (MethodList_t list, SEL op)
{
  MethodList_t method_list = list;

  if (! sel_is_mapped (op))
    return NULL;

  /* If not found then we'll search the list.  */
  while (method_list)
    {
      int i;

      /* Search the method list.  */
      for (i = 0; i < method_list->method_count; ++i)
        {
          Method_t method = &method_list->method_list[i];

          if (method->method_name)
            if (method->method_name->sel_id == op->sel_id)
              return method;
        }

      /* The method wasn't found.  Follow the link to the next list of
         methods.  */
      method_list = method_list->method_next;
    }

  return NULL;
}

static retval_t __objc_forward (id object, SEL sel, arglist_t args);

/* Forwarding pointers/integers through the normal registers */
static id
__objc_word_forward (id rcv, SEL op, ...)
{
  void *args, *res;

  args = __builtin_apply_args ();
  res = __objc_forward (rcv, op, args);
  if (res)
    __builtin_return (res);
  else
    return res;
}

/* Specific routine for forwarding floats/double because of
   architectural differences on some processors.  i386s for
   example which uses a floating point stack versus general
   registers for floating point numbers.  This forward routine 
   makes sure that GCC restores the proper return values */
static double
__objc_double_forward (id rcv, SEL op, ...)
{
  void *args, *res;

  args = __builtin_apply_args ();
  res = __objc_forward (rcv, op, args);
  __builtin_return (res);
}

#if INVISIBLE_STRUCT_RETURN
static __big
#else
static id
#endif
__objc_block_forward (id rcv, SEL op, ...)
{
  void *args, *res;

  args = __builtin_apply_args ();
  res = __objc_forward (rcv, op, args);
  if (res)
    __builtin_return (res);
  else
#if INVISIBLE_STRUCT_RETURN
    return (__big) {{0, 0, 0, 0, 0, 0, 0, 0}};
#else
    return nil;
#endif
}


/* This function is installed in the dispatch table for all methods which are
   not implemented.  Thus, it is called when a selector is not recognized. */
static retval_t
__objc_forward (id object, SEL sel, arglist_t args)
{
  IMP imp;
  static SEL frwd_sel = 0;                      /* !T:SAFE2 */
  SEL err_sel;

  /* first try if the object understands forward:: */
  if (! frwd_sel)
    frwd_sel = sel_get_any_uid ("forward::");

  if (__objc_responds_to (object, frwd_sel))
    {
      imp = get_imp (object->class_pointer, frwd_sel);
      return (*imp) (object, frwd_sel, sel, args);
    }

  /* If the object recognizes the doesNotRecognize: method then we're going
     to send it. */
  err_sel = sel_get_any_uid ("doesNotRecognize:");
  if (__objc_responds_to (object, err_sel))
    {
      imp = get_imp (object->class_pointer, err_sel);
      return (*imp) (object, err_sel, sel);
    }
  
  /* The object doesn't recognize the method.  Check for responding to
     error:.  If it does then sent it. */
  {
    char msg[256 + strlen ((const char *) sel_get_name (sel))
             + strlen ((const char *) object->class_pointer->name)];

    sprintf (msg, "(%s) %s does not recognize %s",
	     (CLS_ISMETA (object->class_pointer)
	      ? "class"
	      : "instance" ),
             object->class_pointer->name, sel_get_name (sel));

    err_sel = sel_get_any_uid ("error:");
    if (__objc_responds_to (object, err_sel))
      {
	imp = get_imp (object->class_pointer, err_sel);
	return (*imp) (object, sel_get_any_uid ("error:"), msg);
      }

    /* The object doesn't respond to doesNotRecognize: or error:;  Therefore,
       a default action is taken. */
    objc_error (object, OBJC_ERR_UNIMPLEMENTED, "%s\n", msg);

    return 0;
  }
}

void
__objc_print_dtable_stats ()
{
  int total = 0;

  objc_mutex_lock (__objc_runtime_mutex);

#ifdef OBJC_SPARSE2
  printf ("memory usage: (%s)\n", "2-level sparse arrays");
#else
  printf ("memory usage: (%s)\n", "3-level sparse arrays");
#endif

  printf ("arrays: %d = %ld bytes\n", narrays, 
	  (long) ((size_t) narrays * sizeof (struct sarray)));
  total += narrays * sizeof (struct sarray);
  printf ("buckets: %d = %ld bytes\n", nbuckets, 
	  (long) ((size_t) nbuckets * sizeof (struct sbucket)));
  total += nbuckets * sizeof (struct sbucket);

  printf ("idxtables: %d = %ld bytes\n",
	  idxsize, (long) ((size_t) idxsize * sizeof (void *)));
  total += idxsize * sizeof (void *);
  printf ("-----------------------------------\n");
  printf ("total: %d bytes\n", total);
  printf ("===================================\n");

  objc_mutex_unlock (__objc_runtime_mutex);
}

/* Returns the uninstalled dispatch table indicator.
 If a class' dispatch table points to __objc_uninstalled_dtable
 then that means it needs its dispatch table to be installed. */
inline
struct sarray *
objc_get_uninstalled_dtable ()
{
  return __objc_uninstalled_dtable;
}

static cache_ptr prepared_dtable_table = 0;

/* This function is called by:
   objc_msg_lookup, get_imp and __objc_responds_to
   (and the dispatch table installation functions themselves)
   to install a dispatch table for a class.

   If CLS is a class, it installs instance methods.
   If CLS is a meta class, it installs class methods.

   In either case +initialize is invoked for the corresponding class.

   The implementation must insure that the dispatch table is not
   installed until +initialize completes.  Otherwise it opens a
   potential race since the installation of the dispatch table is
   used as gate in regular method dispatch and we need to guarantee
   that +initialize is the first method invoked an that no other
   thread my dispatch messages to the class before +initialize
   completes.
 */
static void
__objc_install_dtable_for_class (Class cls)
{
  /* If the class has not yet had its class links resolved, we must 
     re-compute all class links */
  if (! CLS_ISRESOLV (cls))
    __objc_resolve_class_links ();

  /* Make sure the super class has its dispatch table installed
     or is at least preparing.
     We do not need to send initialize for the super class since
     __objc_send_initialize will insure that.
   */
  if (cls->super_class
    && cls->super_class->dtable == __objc_uninstalled_dtable
    && !__objc_prepared_dtable_for_class (cls->super_class))
    {
      __objc_install_dtable_for_class (cls->super_class);
      /* The superclass initialisation may have also initialised the
         current class, in which case there is no more to do. */
      if (cls->dtable != __objc_uninstalled_dtable)
	{
	  return;
	}
    }

  /* We have already been prepared but +initialize hasn't completed.
     The +initialize implementation is probably sending 'self' messages.
     We rely on _objc_get_prepared_imp to retrieve the implementation
     pointers.  */
  if (__objc_prepared_dtable_for_class (cls))
    {
      return;
    }

  /* We have this function cache the implementation pointers
     for _objc_get_prepared_imp but the dispatch table won't
     be initilized until __objc_send_initialize completes.  */
  __objc_prepare_dtable_for_class (cls);

  /* We may have already invoked +initialize but 
     __objc_update_dispatch_table_for_class  invoked by
     class_add_method_list may have reset dispatch table.  */

  /* Call +initialize.
     If we are a real class, we are installing instance methods.
     If we are a meta class, we are installing class methods.
     The __objc_send_initialize itself will insure that the message
     is called only once per class.  */
  if (CLS_ISCLASS (cls))
    __objc_send_initialize (cls);
  else
    {
      /* Retreive the class from the meta class.  */
      Class c = objc_lookup_class (cls->name);
      assert (CLS_ISMETA (cls));
      assert (c);
      __objc_send_initialize (c);
    }

  /* We install the dispatch table correctly when +initialize completed.  */
  __objc_install_prepared_dtable_for_class (cls);
}

/* Builds the dispatch table for the class CLS and stores
   it in a place where it can be retrieved by
   __objc_get_prepared_imp until __objc_install_prepared_dtable_for_class
   installs it into the class.
   The dispatch table should not be installed into the class until
   +initialize has completed.  */
static void
__objc_prepare_dtable_for_class (Class cls)
{
  struct sarray *dtable;
  struct sarray *super_dtable;

  /* This table could be initialized in init.c. 
     We can not use the class name since
     the class maintains the instance methods and
     the meta class maintains the the class methods yet
     both share the same name.  
     Classes should be unique in any program.  */
  if (! prepared_dtable_table)
    prepared_dtable_table 
      = objc_hash_new(32,
		      (hash_func_type) objc_hash_ptr,
		      (compare_func_type) objc_compare_ptrs);

  /* If the class has not yet had its class links resolved, we must 
     re-compute all class links */
  if (! CLS_ISRESOLV (cls))
    __objc_resolve_class_links ();

  assert (cls);
  assert (cls->dtable == __objc_uninstalled_dtable);

  /* If there is already a prepared dtable for this class, we must replace
     it with a new version (since there must have been methods added to or
     otherwise modified in the class while executing +initialize, and the
     table needs to be recomputed.  */
  dtable = __objc_prepared_dtable_for_class (cls);
  if (0 != dtable)
    {
      objc_hash_remove (prepared_dtable_table, cls);
      sarray_free (dtable);
    }

  /* Now prepare the dtable for population.  */
  assert (cls != cls->super_class);
  if (cls->super_class)
    {
      /* Inherit the method list from the super class.
         Yet the super class may still be initializing
	 in the case when a class cluster sub class initializes
	 its super classes.  */
      if (cls->super_class->dtable == __objc_uninstalled_dtable)
	__objc_install_dtable_for_class (cls->super_class);

      super_dtable = cls->super_class->dtable;
      /* If the dispatch table is not yet installed,
	 we are still in the process of executing +initialize.
	 Yet the dispatch table should be available.  */
      if (super_dtable == __objc_uninstalled_dtable)
	super_dtable = __objc_prepared_dtable_for_class (cls->super_class);

      assert (super_dtable);
      dtable = sarray_lazy_copy (super_dtable);
    }
  else
    dtable = sarray_new (__objc_selector_max_index, 0);

  __objc_install_methods_in_dtable (dtable, cls->methods);

  objc_hash_add (&prepared_dtable_table,
		 cls,
		 dtable);
}

/* This wrapper only exists to allow an easy replacement of
   the lookup implementation and it is expected that the compiler
   will optimize it away.  */
static struct sarray *
__objc_prepared_dtable_for_class (Class cls)
{
  struct sarray *dtable = 0;
  assert (cls);
  if (prepared_dtable_table)
    dtable = objc_hash_value_for_key (prepared_dtable_table, cls);
  /* dtable my be nil, 
     since we call this to check whether we are currently preparing
     before we start preparing.  */
  return dtable;
}

/* Helper function for messages sent to CLS or implementation pointers
   retrieved from CLS during +initialize before the dtable is installed.
   When a class implicitly initializes another class which in turn
   implicitly invokes methods in this class, before the implementation of
   +initialize of CLS completes, this returns the expected implementation.
   Forwarding remains the responsibility of objc_msg_lookup.
   This function should only be called under the global lock.
 */
static IMP
__objc_get_prepared_imp (Class cls,SEL sel)
{
  struct sarray *dtable;
  IMP imp;

  assert (cls);
  assert (sel);
  assert (cls->dtable == __objc_uninstalled_dtable);
  dtable = __objc_prepared_dtable_for_class (cls);

  assert (dtable);
  assert (dtable != __objc_uninstalled_dtable);
  imp = sarray_get_safe (dtable, (size_t) sel->sel_id);

  /* imp may be Nil if the method does not exist and we
     may fallback to the forwarding implementation later.  */
  return imp;  
}

/* When this function is called +initialize should be completed.
   So now we are safe to install the dispatch table for the
   class so that they become available for other threads
   that may be waiting in the lock.
 */
static void
__objc_install_prepared_dtable_for_class (Class cls)
{
  assert (cls);
  assert (cls->dtable == __objc_uninstalled_dtable);
  cls->dtable = __objc_prepared_dtable_for_class (cls);

  assert (cls->dtable);
  assert (cls->dtable != __objc_uninstalled_dtable);
  objc_hash_remove (prepared_dtable_table, cls);
}
