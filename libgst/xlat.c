/******************************** -*- C -*- ****************************
 *
 *	Translator to native code.
 *
 *
 ***********************************************************************/

/***********************************************************************
 *
 * Copyright 2001, 2002, 2003, 2004, 2006, 2008 Free Software Foundation, Inc.
 * Written by Paolo Bonzini.
 *
 * This file is part of GNU Smalltalk.
 *
 * GNU Smalltalk is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any later 
 * version.
 * 
 * Linking GNU Smalltalk statically or dynamically with other modules is
 * making a combined work based on GNU Smalltalk.  Thus, the terms and
 * conditions of the GNU General Public License cover the whole
 * combination.
 *
 * In addition, as a special exception, the Free Software Foundation
 * give you permission to combine GNU Smalltalk with free software
 * programs or libraries that are released under the GNU LGPL and with
 * independent programs running under the GNU Smalltalk virtual machine.
 *
 * You may copy and distribute such a system following the terms of the
 * GNU GPL for GNU Smalltalk and the licenses of the other code
 * concerned, provided that you include the source code of that other
 * code when and as the GNU GPL requires distribution of source code.
 *
 * Note that people who make modified versions of GNU Smalltalk are not
 * obligated to grant this special exception for their modified
 * versions; it is their choice whether to do so.  The GNU General
 * Public License gives permission to release a modified version without
 * this exception; this exception also makes it possible to release a
 * modified version which carries forward this exception.
 *
 * GNU Smalltalk is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * GNU Smalltalk; see the file COPYING.  If not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.  
 *
 ***********************************************************************/

#include "gstpriv.h"
#include "match.h"

#ifdef ENABLE_JIT_TRANSLATION

/* This file implements GNU Smalltalk's just-in-time compiler to native code.
   It is inspired by techniques shown in Ian Piumarta's PhD thesis "Delayed
   code generation in a Smalltalk-80 compiler" (available online at
   http://www-sor.inria.fr/~piumarta), with quite a few modifications:
    - we target a RISC architecture (GNU lightning) instead of the CISC 
      Motorola 68020 architecture used in the thesis.
    - we use inline caching (only discussed briefly in the thesis)
    - block implementation is radically different
    - we don't work directly on a parse tree.  Rather, we recreate a tree
      structure from bytecodes that constitute a statement.
    - a few parts that were left back in the thesis (doesNotUnderstand:,
      non local returns, primitives, etc.) are fully implemented
  
   The aim of the code in this file is to generate pretty good code,
   as fast as possible, and without requiring too much memory and
   information (Self's 64MB requirement comes to mind...).  Nothing
   less, nothing more.  All bottlenecks and inefficiencies should be
   due to the generic nature of GNU lightning's architecture and to
   interp.c, not to the compiler.  */


/* This structure represents an n-tree. Children of a node are
   connected by a linked list. It is probably the most important for
   the operation of the translator.
 
   The translator operates on blocks of bytecodes that constitute a
   statement, and represents what the block does on a stack.  The
   stack, however, does not contain results of the execution, but
   rather code_trees that store how the value in that stack slot was
   computed; these code_trees are built by dcd_* functions.  When a
   statement boundary is found (i.e.  a result on the stack is
   discarded, a jump is encountered, or a jump destination is
   reached), the code_trees that are currently on the stack are walked
   (calling recursively the gen_* functions depending on the contents
   of the 'operation' field), resulting in the generation of native
   code.  */
typedef struct code_tree
{
  struct code_tree *child, *next;
  int operation;
  PTR data;
  jit_node_t *jumpDest;
  gst_uchar *bp;
  unsigned char bc_len;		/* only used for sends */
} code_tree, *code_stack_element, **code_stack_pointer;

/* This structure represents a message send.  A sequence of
   inline_cache objects is allocated on the heap and initialized as
   the code_tree is constructed.  Inline caches have two roles:
  
   a) avoiding that data is stored in the methodsTableObstack, therefore
      making it easier to guess the size of the produced native code
 
   b) improving execution speed by lowering the number of global cache
      lookups to be done.
  
   A pointer to an inline_cache is used for the 'data' field in
   message send code_trees.  */
typedef struct inline_cache
{
  OOP selector;
  void *cachedIP;
  void *native_ip;
  char imm;			/* For short sends, the selector number. */
  char numArgs;
  char more;
  char is_super;
}
inline_cache;

typedef struct ip_map
{
  void *native_ip;
  int virtualIP;
}
ip_map;

/* This structure forms a list that remembers which message sends were
   inlined directly into the instruction flow.  The list is walked by
   emit_deferred_sends after the last bytecode has been compiled, and
   recovery code that performs real message sends is written.  */
typedef struct deferred_send
{
  code_tree *tree;
  jit_node_t *trueDest;
  jit_node_t *falseDest;
  jit_node_t *address;
  int reg0, reg1;
  OOP oop;
  struct deferred_send *next;
}
deferred_send;

typedef struct native_entry {
    struct native_entry *next;
    void *code;
    method_entry *method;
} native_entry;


/* Lighting */
static jit_state_t *global_jit, *_jit;
static jit_node_t **map_patch;
static ip_map *buf_patch;
static size_t num_map_patch, size_map_patch;
static size_t num_buf_patch, size_buf_patch;

/* To reduce multiplies and divides to shifts */

#define LONG_SHIFT (sizeof (long) == 4 ? 2 : 3)

/* These are for the hash table of translations */

#define HASH_TABLE_SIZE		(8192)

/* Here is where the dynamically compiled stuff goes */
static method_entry *methods_table[HASH_TABLE_SIZE+1], *released;
static native_entry *native_table[HASH_TABLE_SIZE];

#define discarded methods_table[HASH_TABLE_SIZE]

/* Current status of the translator at the method level */
static method_entry *current;
static struct obstack aux_data_obstack;
static inline_cache *curr_inline_cache;
static deferred_send *deferred_head;
static jit_node_t **labels, **this_label;
static gst_uchar *bc;
static OOP *literals;
static OOP method_class;
static code_stack_element t_stack[MAX_DEPTH];
static code_stack_pointer t_sp;

/* Current status of the code generator */
static mst_Boolean self_cached, rec_var_cached;
static int sp_delta, self_class_check, stack_cached;

/* These are pieces of native code that are used by the run-time.  */
static void *do_send_code, *do_super_code, *non_boolean_code,
  *bad_return_code, *does_not_understand_code;
static jit_node_t *native_code_label;
#if 1
char method_name[64];
#define	jit_field(struc, f)		( ((long) (&((struc *) 8)->f) ) - 8)
#define	jit_ptr_field(struc_p, f)	( ((long) (&((struc_p) 8)->f) ) - 8)
#endif
#define DISASSEMBLE		0

PTR (*_gst_run_native_code) ();
PTR (*_gst_return_from_native_code) ();



/* Kinds of functions used in function tables */
typedef void (*emit_func) (code_tree *);
typedef mst_Boolean (*decode_func) (gst_uchar b, gst_uchar *bp);

/* Constants used in the reconstruction of the parse tree (operation field)
  
   .---------------. .--------------.-----------.--------------.
   |   bits 12-13  |.|   bits 6-8   | bits 3-5  |  bits 0-2    |
   |---------------|.|--------------|-----------|--------------|
   |  class check  |.| jump, pop &  | operation | suboperation |
   | SmallInteger  |.| return flags |           |              |
   '---------------' '--------------'-----------'--------------'
                    \
                     \__ 3 unused bits */

/* operations 				** value of tree->data		*/
#define TREE_OP			00070
#define TREE_SEND		00000	/* points to an inline_cache */
#define TREE_STORE		00010	/* see below */
#define TREE_PUSH		00020	/* see below */
#define TREE_ALT_PUSH		00030	/* see below */
#define TREE_SET_TOP		00040	/* see below */
#define TREE_NOP		00050	/* unused */

/* suboperations for TREE_SEND */
#define TREE_SUBOP		00007
#define TREE_NORMAL		00000
#define TREE_BINARY_INT		00001
#define TREE_BINARY_BOOL	00003	/* 2 skipped - reserved to LIT_CONST */
#define TREE_UNARY_SPECIAL	00004
#define TREE_UNARY_BOOL		00005
#define TREE_STORE_LIT_VAR	00006	/* receiver in V1 */
#define TREE_DIRTY_BLOCK	00007	/* doesn't use tree->data! */

/* stack suboperations 			   value of tree->data		*/
#define TREE_REC_VAR		00000	/* variable number */
#define TREE_TEMP		00001	/* variable number */
#define TREE_LIT_CONST		00002	/* literal to be pushed */
#define TREE_LIT_VAR		00003	/* An gst_association object */
#define TREE_DUP		00004	/* unused */
#define TREE_SELF		00005	/* unused */
#define TREE_OUTER_TEMP		00006	/* unused */
#define TREE_POP_INTO_ARRAY	00007	/* index */

/* suboperations for TREE_NOP */
#define TREE_ALREADY_EMITTED	00000
#define TREE_TWO_EXTRAS		00001

/* extra operations */
#define TREE_EXTRA		00700
#define TREE_EXTRA_NONE		00000
#define TREE_EXTRA_JMP_TRUE	00100
#define TREE_EXTRA_JMP_FALSE	00200
#define TREE_EXTRA_JMP_ALWAYS	00300
#define TREE_EXTRA_RETURN	00400
#define TREE_EXTRA_METHOD_RET	00500
#define TREE_EXTRA_POP		00600

/* class check flags */
#define TREE_CLASS_CHECKS	0x03000L
#define TREE_IS_INTEGER		0x01000L
#define TREE_IS_NOT_INTEGER	0x02000L

/* testing macros */
#define NOT_INTEGER(tree) ( (tree)->operation & TREE_IS_NOT_INTEGER)
#define IS_INTEGER(tree)  ( (tree)->operation & TREE_IS_INTEGER)
#define IS_PUSH(tree)	  ( ((tree)->operation & TREE_OP) == TREE_PUSH)
#define IS_SEND(tree)	  ( ((tree)->operation & TREE_OP) == TREE_SEND)
#define IS_STORE(tree)	  ( ((tree)->operation & TREE_OP) == TREE_STORE)
#define IS_SET_TOP(tree)  ( ((tree)->operation & TREE_OP) == TREE_SET_TOP)
#define IS_LITERAL(tree)  ( ((tree)->operation & TREE_SUBOP) == TREE_LIT_CONST)



/* Strength reduction */
static inline int analyze_factor (int x);
static inline void analyze_dividend (int imm, int *shift, mst_Boolean *adjust, uintptr_t *factor);

/* address translation */
static inline void defer_map_patch (void **label);
static inline void defer_buf_patch (size_t offset);
static void flush_patches (char *buf);

/* label handling */
static inline void define_ip_map_entry (int virtualIP);

/* Inlining (deferred sends) */
static void defer_send (code_tree *tree, mst_Boolean isBool, jit_node_t *jump, int reg0, int reg1, OOP oop);
static inline jit_node_t *last_deferred_send (void);
static inline void emit_deferred_sends (deferred_send *ds);
static inline void finish_deferred_send (void);

/* CompiledMethod hash table handling */
static method_entry *find_method_entry (OOP methodOOP, OOP receiverClass);
static inline void new_method_entry (OOP methodOOP, OOP receiverClass);
static inline method_entry *finish_method_entry (void);
static void add_native_entry (method_entry *entry);
static void rem_native_entry (method_entry *entry);

/* code_tree handling */
static inline code_tree *push_tree_node (gst_uchar *bp, code_tree *firstChild, int operation, PTR data);
static inline code_tree *push_tree_node_oop (gst_uchar *bp, code_tree *firstChild, int operation, OOP literal);
static inline code_tree *pop_tree_node (code_tree *linkedChild);
static inline code_tree *push_send_node (gst_uchar *bp, unsigned char bc_len, OOP selector, int numArgs, mst_Boolean super, int operation, int imm);
static inline void set_top_node_extra (int extra, int jumpOffset);
static inline gst_uchar *decode_bytecode (gst_uchar *bp);

static inline void emit_code (void);
static void emit_code_tree (code_tree *tree);

/* Non-bytecode specific code generation functions */
static inline void emit_user_defined_method_call (OOP methodOOP, int numArgs, gst_compiled_method method);
static inline mst_Boolean emit_method_prolog (OOP methodOOP, gst_compiled_method method);
static inline mst_Boolean emit_block_prolog (OOP blockOOP, gst_compiled_block block);
static inline mst_Boolean emit_inlined_primitive (int primitive, int numArgs, int attr);
static inline mst_Boolean emit_primitive (int primitive, int numArgs);

static inline void emit_interrupt_check (int restartReg, int ipOffset);
static inline void generate_run_time_code (void);
static inline void translate_method (OOP methodOOP, OOP receiverClass, int size);
static void emit_basic_size_in_r0 (OOP classOOP, mst_Boolean tagged, int objectReg);

/* Code generation functions for bytecodes */
static void gen_send (code_tree *tree);
static void gen_binary_int (code_tree *tree);
static void gen_pop_into_array (code_tree *tree);
static void gen_binary_bool (code_tree *tree);
static void gen_send_store_lit_var (code_tree *tree);
static void gen_dirty_block (code_tree *tree);
static void gen_unary_special (code_tree *tree);
static void gen_unary_bool (code_tree *tree);
static void gen_store_rec_var (code_tree *tree);
static void gen_store_temp (code_tree *tree);
static void gen_store_lit_var (code_tree *tree);
static void gen_store_outer (code_tree *tree);
static void gen_push_rec_var (code_tree *tree);
static void gen_push_temp (code_tree *tree);
static void gen_push_lit_const (code_tree *tree);
static void gen_push_lit_var (code_tree *tree);
static void gen_dup_top (code_tree *tree);
static void gen_push_self (code_tree *tree);
static void gen_push_outer (code_tree *tree);
static void gen_top_rec_var (code_tree *tree);
static void gen_top_temp (code_tree *tree);
static void gen_top_self (code_tree *tree);
static void gen_top_outer (code_tree *tree);
static void gen_alt_rec_var (code_tree *tree);
static void gen_alt_temp (code_tree *tree);
static void gen_alt_lit_const (code_tree *tree);
static void gen_alt_lit_var (code_tree *tree);
static void gen_get_top (code_tree *tree);
static void gen_alt_self (code_tree *tree);
static void gen_alt_outer (code_tree *tree);
static void gen_top_lit_const (code_tree *tree);
static void gen_top_lit_var (code_tree *tree);
static void gen_nothing (code_tree *tree);
static void gen_two_extras (code_tree *tree);
static void gen_invalid (code_tree *tree);

/* Function table for the code generator */
static const emit_func emit_operation_funcs[96] = {
  gen_send, gen_binary_int, gen_invalid, gen_binary_bool,
  gen_unary_special, gen_unary_bool, gen_send_store_lit_var, gen_dirty_block,

  gen_store_rec_var, gen_store_temp, gen_invalid, gen_store_lit_var,
  gen_invalid, gen_invalid, gen_store_outer, gen_pop_into_array,

  gen_push_rec_var, gen_push_temp, gen_push_lit_const, gen_push_lit_var,
  gen_dup_top, gen_push_self, gen_push_outer, gen_invalid,

  gen_alt_rec_var, gen_alt_temp, gen_alt_lit_const, gen_alt_lit_var,
  gen_get_top, gen_alt_self, gen_alt_outer, gen_invalid,

  gen_top_rec_var, gen_top_temp, gen_top_lit_const, gen_top_lit_var,
  gen_invalid, gen_top_self, gen_top_outer, gen_invalid,

  gen_nothing, gen_two_extras, gen_invalid, gen_invalid,
  gen_invalid, gen_invalid, gen_invalid, gen_invalid
};

static const int special_send_bytecodes[32] = {
  TREE_SEND | TREE_BINARY_INT,		/* PLUS_SPECIAL */
  TREE_SEND | TREE_BINARY_INT,		/* MINUS_SPECIAL */
  TREE_SEND | TREE_BINARY_BOOL,		/* LESS_THAN_SPECIAL */
  TREE_SEND | TREE_BINARY_BOOL,		/* GREATER_THAN_SPECIAL */
  TREE_SEND | TREE_BINARY_BOOL,		/* LESS_EQUAL_SPECIAL */
  TREE_SEND | TREE_BINARY_BOOL,		/* GREATER_EQUAL_SPECIAL */
  TREE_SEND | TREE_BINARY_BOOL,		/* EQUAL_SPECIAL */
  TREE_SEND | TREE_BINARY_BOOL,		/* NOT_EQUAL_SPECIAL */
  TREE_SEND | TREE_BINARY_INT,		/* TIMES_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* DIVIDE_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* REMAINDER_SPECIAL */
  TREE_SEND | TREE_BINARY_INT,		/* BIT_XOR_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* BIT_SHIFT_SPECIAL */
  TREE_SEND | TREE_BINARY_INT,		/* INTEGER_DIVIDE_SPECIAL */
  TREE_SEND | TREE_BINARY_INT,		/* BIT_AND_SPECIAL */
  TREE_SEND | TREE_BINARY_INT,		/* BIT_OR_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* AT_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* AT_PUT_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* SIZE_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* CLASS_SPECIAL */
  TREE_SEND | TREE_UNARY_BOOL,		/* IS_NIL_SPECIAL */
  TREE_SEND | TREE_UNARY_BOOL,		/* NOT_NIL_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* VALUE_SPECIAL */
  TREE_SEND | TREE_NORMAL,		/* VALUE_COLON_SPECIAL */
  TREE_SEND | TREE_BINARY_BOOL,		/* SAME_OBJECT_SPECIAL */
  TREE_SEND | TREE_UNARY_SPECIAL,	/* JAVA_AS_INT_SPECIAL */
  TREE_SEND | TREE_UNARY_SPECIAL,	/* JAVA_AS_LONG_SPECIAL */
};


/* Runtime support code */

static void
generate_dnu_code (void)
{
  /* send #doesNotUnderstand: If the method is not understood, the
     stack is changed to the format needed by #doesNotUnderstand: in
     lookup_native_ip; no inline caching must take place because we
     have modify the stack each time they try to send the message.  */

  jit_ldi (JIT_V2, &sp);	/* changed by lookup_method!! */
  jit_movi (JIT_R2, 1);
  jit_ldi (JIT_R0, &_gst_does_not_understand_symbol);
  jit_ldxi (JIT_R1, JIT_V2, -(int)sizeof (PTR));
  jit_prepare ();
  jit_pushargr (JIT_R0);	/* selector */
  jit_pushargr (JIT_R2);	/* numArgs */
  jit_pushargr (JIT_R1);	/* receiver */
  jit_pushargr (JIT_V0);	/* method_class */
  jit_finishi (PTR_LOOKUP_NATIVE_IP);
  jit_retval (JIT_R0);

  /* Could crash if again #doesNotUnderstand: -- probably better than
     an infinite loop.  */
  jit_jmpr (JIT_R0);
}

static void
generate_bad_return_code (jit_node_t *does_not_understand_label)
{
  jit_node_t *jmp;

  jit_ldi (JIT_V2, &sp);
  jit_movi (JIT_R2, 0);
  jit_ldi (JIT_R0, &_gst_bad_return_error_symbol);
  jit_ldr (JIT_R1, JIT_V2);

  /* load the class of the receiver (which is in R1) */
  jit_movi (JIT_V0, (jit_word_t)_gst_small_integer_class);
  jmp = jit_bmsi (JIT_R1, 1);
  jit_ldxi (JIT_V0, JIT_R1, offsetof (struct oop_s, object));
  jit_ldxi (JIT_V0, JIT_V0, offsetof (struct object_s, objClass));
#if DISASSEMBLE
  jit_note("badReturn", __LINE__);
#endif
  jit_patch (jmp);

  jit_prepare ();
  jit_pushargr (JIT_R0);	/* selector */
  jit_pushargr (JIT_R2);	/* numArgs */
  jit_pushargr (JIT_R1);	/* receiver */
  jit_pushargr (JIT_V0);	/* method_class */
  jit_finishi (PTR_LOOKUP_NATIVE_IP);
  jit_retval (JIT_R0);

  /* Might not be understood... how broken they are :-) */
  jmp = jit_beqi (JIT_R0, 0);
  jit_patch_at (jmp, does_not_understand_label);
#if DISASSEMBLE
  jit_note("badReturn", __LINE__);
#endif
  jit_jmpr (JIT_R0);
}

static void
generate_non_boolean_code (void)
{
  static char methodName[] = "mustBeBoolean";

  jit_ldi (JIT_V2, &sp);	/* push R0 on the */
  jit_stxi (sizeof (PTR), JIT_V2, JIT_R0);	/* Smalltalk stack */
  jit_addi (JIT_V2, JIT_V2, sizeof (PTR));
  jit_movi (JIT_R1, (jit_word_t)methodName);
  jit_sti (&sp, JIT_V2);	/* update SP */
  jit_sti (&_gst_abort_execution, JIT_R1);
  jit_ret ();
}

static void
generate_do_super_code (jit_node_t *does_not_understand_label)
{
  jit_node_t *jmp;

  /* load other args into R1/R2 */
  jit_ldi (JIT_R1, &_gst_self);
  jit_ldxi_uc (JIT_R2, JIT_V1, offsetof (inline_cache, numArgs));
  jit_ldxi (JIT_R0, JIT_V1, offsetof (inline_cache, selector));

  jit_prepare ();
  jit_pushargr (JIT_R0);	/* selector */
  jit_pushargr (JIT_R2);	/* numArgs */
  jit_pushargr (JIT_R1);	/* receiver */
  jit_pushargr (JIT_V0);	/* method_class */
  jit_finishi (PTR_LOOKUP_NATIVE_IP);
  jit_retval (JIT_R0);

  /* store the address in the inline cache if not #doesNotUnderstand: */
  jmp = jit_beqi (JIT_R0, 0);
  jit_patch_at (jmp, does_not_understand_label);
  jit_stxi (offsetof (inline_cache, cachedIP), JIT_V1, JIT_R0);
#if DISASSEMBLE
  jit_note("doSuper", __LINE__);
#endif
  jit_jmpr (JIT_R0);
}

static void
generate_do_send_code (jit_node_t *does_not_understand_label)
{
  jit_node_t *jmp;

  /* load other parameters into R0/R2 */
  jit_ldxi_uc (JIT_R2, JIT_V1, offsetof (inline_cache, numArgs));
  jit_ldxi (JIT_R0, JIT_V1, offsetof (inline_cache, selector));

  /* load _gst_self into R1 */
  jit_lshi (JIT_R1, JIT_R2, LONG_SHIFT);
  jit_negr (JIT_R1, JIT_R1);
  jit_ldxr (JIT_R1, JIT_V2, JIT_R1);

  /* method class */
  jit_movi (JIT_V0, (jit_word_t)_gst_small_integer_class);
  jmp = jit_bmsi (JIT_R1, 1);
  jit_ldxi (JIT_V0, JIT_R1, offsetof (struct oop_s, object));
  jit_ldxi (JIT_V0, JIT_V0, offsetof (struct object_s, objClass));
#if DISASSEMBLE
  jit_note("doSend", __LINE__);
#endif
  jit_patch (jmp);

  jit_prepare ();
  jit_pushargr (JIT_R0);	/* selector */
  jit_pushargr (JIT_R2);	/* numArgs */
  jit_pushargr (JIT_R1);	/* receiver */
  jit_pushargr (JIT_V0);	/* method_class */
  jit_finishi (PTR_LOOKUP_NATIVE_IP);
  jit_retval (JIT_R0);

  /* store the address in the inline cache if not #doesNotUnderstand: */
  jmp = jit_beqi (JIT_R0, 0);
  jit_patch_at (jmp, does_not_understand_label);
  jit_stxi (offsetof (inline_cache, cachedIP), JIT_V1, JIT_R0);
#if DISASSEMBLE
  jit_note("doSend", __LINE__);
#endif
  jit_jmpr (JIT_R0);
}

void
generate_run_time_code (void)
{
  static inline_cache ic;
  jit_node_t *arg;
  jit_node_t *does_not_understand_label;
  jit_node_t *bad_return_label;
  jit_node_t *non_boolean_label;
  jit_node_t *do_super_label;
  jit_node_t *do_send_label;
  jit_node_t *return_zero_from_native_label;
  jit_node_t *return_from_native_label;

  size_map_patch = 16;
  map_patch = (jit_node_t **) xmalloc (sizeof (jit_node_t *) * size_map_patch);
  num_map_patch = 0;

  size_buf_patch = 16;
  buf_patch = (ip_map *) xmalloc (sizeof (ip_map) * size_buf_patch);
  num_buf_patch = 0;

#if 0
  init_jit (NULL);
#else
  init_jit ("/home/pcpa/github/smalltalk/.libs/gst");
#endif
  _jit = global_jit = jit_new_state();

#if DISASSEMBLE
  jit_note("runNative", __LINE__);
#endif
  jit_prolog ();
  jit_frame (256);
  arg = jit_arg ();
  jit_getarg (JIT_R0, arg);
  jit_movi (JIT_V1, (jit_word_t)&ic);
  jit_ldi (JIT_V2, &sp);
  jit_jmpr (JIT_R0);

#if DISASSEMBLE
  jit_note("doesNotUnderstand", __LINE__);
#endif
  does_not_understand_label = jit_indirect ();
  generate_dnu_code ();

  /* send #badReturnError.  No inline caching must take place because
     this is called upon a return, not upon a send.  */
#if DISASSEMBLE
  jit_note("badReturn", __LINE__);
#endif
  bad_return_label = jit_indirect ();
  generate_bad_return_code (does_not_understand_label);

#if DISASSEMBLE
  jit_note("nonBoolean", __LINE__);
#endif
  non_boolean_label = jit_indirect ();
  generate_non_boolean_code ();

#if DISASSEMBLE
  jit_note("doSuper", __LINE__);
#endif
  do_super_label = jit_indirect ();
  generate_do_super_code (does_not_understand_label);

#if DISASSEMBLE
  jit_note("doSend", __LINE__);
#endif
  do_send_label = jit_indirect ();
  generate_do_send_code (does_not_understand_label);

#if DISASSEMBLE
  jit_note("returnZeroFromNative", __LINE__);
#endif
  return_zero_from_native_label = jit_indirect ();
  jit_movi (JIT_R0, 0);
#if DISASSEMBLE
  jit_note("returnFromNative", __LINE__);
#endif
  return_from_native_label = jit_indirect ();
  jit_retr (JIT_R0);

  jit_realize ();
#if !DISASSEMBLE
  jit_set_data (NULL, 0, JIT_DISABLE_DATA|JIT_DISABLE_NOTE);
#endif
  _gst_run_native_code = jit_emit ();

  does_not_understand_code = jit_address (does_not_understand_label);
  bad_return_code = jit_address (bad_return_label);
  non_boolean_code = jit_address (non_boolean_label);
  do_send_code = jit_address (do_send_label);
  do_super_code = jit_address (do_super_label);
  non_boolean_code = jit_address (non_boolean_label);
  bad_return_code = jit_address (bad_return_label);
  does_not_understand_code = jit_address (does_not_understand_label);
  _gst_return_from_native_code = jit_address (return_from_native_label);
  ic.native_ip = jit_address (return_zero_from_native_label);

#if DISASSEMBLE
  printf ("<main>\n");
  jit_print ();
  jit_disassemble ();
  fflush (stdout);
#endif
  jit_clear_state ();
}


/* Functions for managing the translated methods' hash table */

void
new_method_entry (OOP methodOOP, OOP receiverClass)
{
  current = (method_entry *) xmalloc (sizeof (method_entry));
  current->methodOOP = methodOOP;
  current->receiverClass = receiverClass;
  current->inlineCaches = NULL;
  methodOOP->flags |= F_XLAT;

#if DISASSEMBLE
  printf ("<methodOOP: ");
  _gst_print_object (methodOOP);
  printf (">\n<receiverClass: ");
  _gst_print_object (receiverClass);
  printf (">\n");
  sprintf (method_name, "%lx", methodOOP);
#endif

  /* The buffer functions in str.c are used to deal with the ip_map.  */
  _gst_reset_buffer ();

  obstack_init (&aux_data_obstack);
  current->_jit = _jit = jit_new_state ();

  /* Create code in trampoline format that GNU lightning understands. */
  native_code_label = jit_label ();
  jit_prolog ();
  jit_tramp (256);
}

method_entry *
finish_method_entry (void)
{
  unsigned int hashEntry;
  method_entry *result;
  int size;

  /* Close the trampoline associated with the method entry. */
  jit_epilog ();

  result = current;
  current = NULL;

  /* Copy the IP map, adding a final dummy entry */
  define_ip_map_entry (-1);
  jit_realize ();
#if !DISASSEMBLE
  jit_set_data (NULL, 0, JIT_DISABLE_DATA|JIT_DISABLE_NOTE);
#endif
  result->nativeCode = jit_emit ();
  size = _gst_buffer_size ();
  result->ipMap = (ip_map *) xmalloc (size);
  _gst_copy_buffer (result->ipMap);
  flush_patches ((char *)result->ipMap);
  //
#if DISASSEMBLE
  jit_print ();
  jit_disassemble ();
  fflush (stdout);
#endif
  //
  jit_clear_state ();

  hashEntry = OOP_INDEX (result->methodOOP) % HASH_TABLE_SIZE;
  result->next = methods_table[hashEntry];
  methods_table[hashEntry] = result;
  add_native_entry (result);

  obstack_free (&aux_data_obstack, NULL);
  return result;
}

static void
add_native_entry (method_entry *method)
{
  native_entry *native;
  unsigned int hash;

  assert (method->nativeCode);
  hash = ((unsigned long)method->nativeCode >> LONG_SHIFT) % HASH_TABLE_SIZE;
  native = (native_entry *) xmalloc (sizeof (native_entry));
  native->code = method->nativeCode;
  native->method = method;
  native->next = native_table[hash];
  native_table[hash] = native;
}

method_entry *
get_native_entry (void *code)
{
  native_entry *native;
  unsigned int hash;

  hash = ((unsigned long)code >> LONG_SHIFT) % HASH_TABLE_SIZE;
  for (native = native_table[hash]; native; native = native->next)
    {
      if (native->code == code)
	break;
    }
  return native ? native->method : NULL;
}

static void
rem_native_entry (method_entry *method)
{
  native_entry *prev, *native;
  unsigned int hash;

  hash = ((unsigned long)method->nativeCode >> LONG_SHIFT) % HASH_TABLE_SIZE;
  prev = native = native_table[hash];
  for (; native; prev = native, native = native->next)
    {
      if (native->code == method->nativeCode)
	break;
    }
  assert (native);
  if (prev == native)
    native_table[hash] = native->next;
  else
    prev->next = native->next;
  xfree (native);
}


/* Functions for managing the code_tree */

code_tree *
push_tree_node (gst_uchar *bp, code_tree *firstChild, int operation, PTR data)
{
  code_tree *node =
    obstack_alloc (&aux_data_obstack, sizeof (code_tree));

  node->child = firstChild;
  node->next = NULL;
  node->operation = operation;
  node->data = data;
  node->bp = bp;
  *t_sp++ = node;
  return (node);
}

code_tree *
push_tree_node_oop (gst_uchar *bp, code_tree *firstChild, int operation, OOP literal)
{
  int classCheck;
  if (IS_INT (literal))
    classCheck = TREE_IS_INTEGER;
  else
    classCheck = TREE_IS_NOT_INTEGER;

  return push_tree_node (bp, firstChild, operation | classCheck,
			 literal);
}

code_tree *
pop_tree_node (code_tree *linkedChild)
{
  if (t_sp <= t_stack)
    {
      /* Stack underflow (note that it can be legal in a few cases,
         such as for return stack top bytecodes) */
      return (NULL);
    }
  else
    {
      code_tree *node = *--t_sp;
      node->next = linkedChild;
      return (node);
    }
}

void
set_top_node_extra (int extra, int jumpOffset)
{
  code_tree *node;

#ifndef OPTIMIZE
  if (extra == TREE_EXTRA_JMP_ALWAYS
      || extra == TREE_EXTRA_JMP_TRUE
      || extra == TREE_EXTRA_JMP_FALSE)
    assert (this_label[jumpOffset] != NULL);
#endif

  if (t_sp <= t_stack)
    {
      /* Stack is currently empty -- generate the code directly */
      if (extra != TREE_EXTRA_JMP_ALWAYS)
	{
	  OOP selector = GET_METHOD_SELECTOR (current->methodOOP);
	  if (method_class == current->receiverClass)
	    _gst_errorf ("Stack underflow in JIT compilation %O>>%O",
		         current->receiverClass, selector);
	  else
	    _gst_errorf ("Stack underflow in JIT compilation %O(%O)>>%O",
		         current->receiverClass, method_class, selector);

	  abort ();
	}
      node = alloca (sizeof (code_tree));

      node->child = node->next = NULL;
      node->operation = TREE_NOP | TREE_ALREADY_EMITTED | extra;
      node->jumpDest = this_label[jumpOffset];
      emit_code_tree (node);
      return;
    }

  node = t_sp[-1];
  if (node->operation & TREE_EXTRA)
    {
      /* More than one extra operation -- add a fake node */
      node = obstack_alloc (&aux_data_obstack, sizeof (code_tree));
      node->child = NULL;
      node->next = t_sp[-1];
      node->operation = TREE_NOP | TREE_TWO_EXTRAS;
      t_sp[-1] = node;
    }

  node->operation |= extra;
  node->jumpDest = this_label[jumpOffset];
}

static inline inline_cache *
set_inline_cache (OOP selector, int numArgs, mst_Boolean super, int operation, int imm)
{
  curr_inline_cache->numArgs = numArgs;
  curr_inline_cache->selector = selector;
  curr_inline_cache->cachedIP = super ? do_super_code : do_send_code;
  curr_inline_cache->is_super = super;
  curr_inline_cache->more = true;
  curr_inline_cache->imm = imm;
  return curr_inline_cache++;
}

code_tree *
push_send_node (gst_uchar *bp, unsigned char bc_len, OOP selector, int numArgs, mst_Boolean super, int operation, int imm)
{
  code_tree *args, *node;
  int tot_args;
  inline_cache *ic = set_inline_cache (selector, numArgs, super, operation, imm);

  /* Remember that we must pop an extra node for the receiver! */
  tot_args = numArgs + (super ? 2 : 1);
  for (args = NULL; tot_args--;)
    args = pop_tree_node (args);

  node = push_tree_node (bp, args, operation, (PTR) ic);
  node->bc_len = bc_len;
  return (node);
}

void
emit_code (void)
{
  code_tree **pTree, *tree;

  for (pTree = t_stack; pTree < t_sp; pTree++)
    {
      tree = *pTree;
      emit_code_tree (tree);
    }

  rec_var_cached = false;
  stack_cached = -1;
  self_cached = false;
}


/* A couple of commodities for strength reduction */

int
analyze_factor (int x)
{
  int a;
  int b, c;

  a = x & (x - 1);		/* clear lowest bit */
  a &= a - 1;			/* again */

  if (a)			/* more than two bits are set to 1 */
    return 0;			/* don't attempt strength reduction */

  for (b = 0; (x & 1) == 0; b++, x >>= 1);
  if (x == 1)
    return b;			/* a single bit was set */

  for (c = b + 1; (x & 2) == 0; c++, x >>= 1);
  return b | (c << 8);
}

void
analyze_dividend (int imm, int *shift, mst_Boolean *adjust, uintptr_t *factor)
{
  int x, b, r;
  double f;

  *adjust = 0;

  /* compute floor(log2 imm) */
  for (r = 0, x = imm >> 1; x; r++, x >>= 1);

  if (!(imm & (imm - 1)))
    {
      /* x is a power of two */
      *shift = r;
      *factor = 0;
      return;
    }

  r += 31;
  f = ldexp (((double) 1.0) / imm, r);
  b = (int) floor (f);

  if ((f - (double) b) < 0.5)
    /* round f down to nearest integer, compute ((x + 1) * f) >> r */
    ++*adjust;
  else
    /* round f up to nearest integer, compute (x * f) >> r */
    ++b;

  /* Try to shift less bits */
  while ((r >= 32) && ((b & 1) == 0))
    {
      r--;
      b >>= 1;
    }

  *factor = b;
  *shift = r - 32;
}


/* Functions for managing labels and forward references */
void
defer_map_patch (void **label)
{
  if (num_map_patch + 1 >= size_map_patch)
    {
      size_map_patch += 16;
      map_patch = (jit_node_t **) xrealloc (map_patch, size_map_patch
					    * sizeof (jit_node_t *));
    }
  map_patch[num_map_patch++] = (jit_node_t *)label;
  *label = jit_indirect ();
}

void
defer_buf_patch (size_t offset)
{
  if (num_buf_patch + 1 >= size_buf_patch)
    {
      size_buf_patch += 16;
      buf_patch = (ip_map *) xrealloc (buf_patch, size_buf_patch
				       * sizeof (ip_map));
    }
  buf_patch[num_buf_patch].native_ip = jit_indirect ();
  buf_patch[num_buf_patch].virtualIP = offset;
  ++num_buf_patch;
}

void
flush_patches (char *buf)
{
  size_t i;
  for (i = 0; i < num_map_patch; ++i)
    *(void **)map_patch[i] = jit_address(*(void **)map_patch[i]);
  num_map_patch = 0;

  for (i = 0; i < num_buf_patch; ++i)
    *(void **)(buf + buf_patch[i].virtualIP) =
      jit_address (buf_patch[i].native_ip);
  num_buf_patch = 0;
}

void
define_ip_map_entry (int virtualIP)
{
  ip_map mapEntry;
  mapEntry.virtualIP = virtualIP;
  defer_buf_patch (_gst_buffer_size ());

  _gst_add_buf_data (&mapEntry, sizeof (mapEntry));
}

void
finish_deferred_send (void)
{
  if (!deferred_head->trueDest)
    {
      deferred_head->trueDest = jit_label ();
      if (!deferred_head->falseDest)
	deferred_head->falseDest = deferred_head->trueDest;
    }

  else if (!deferred_head->falseDest)
    deferred_head->falseDest = jit_label ();
}

jit_node_t *
last_deferred_send (void)
{
  return deferred_head->address;
}

void
defer_send (code_tree *tree, mst_Boolean isBool, jit_node_t *jump, int reg0, int reg1, OOP oop)
{
  deferred_send *ds =
    obstack_alloc (&aux_data_obstack, sizeof (deferred_send));

  if (isBool)
    {
      switch (tree->operation & TREE_EXTRA)
	{
	case TREE_EXTRA_NONE:
	case TREE_EXTRA_POP:
	case TREE_EXTRA_RETURN:
	case TREE_EXTRA_METHOD_RET:
	case TREE_EXTRA_JMP_ALWAYS:
	  isBool = false;
	}
    }

  ds->next = deferred_head;
  ds->tree = tree;
  ds->reg0 = reg0;
  ds->reg1 = reg1;
  ds->oop = oop;
  ds->address = jit_forward ();

  if (jump)
    jit_patch_at (jump, ds->address);

  if (isBool)
    {
      if ((tree->operation & TREE_EXTRA) == TREE_EXTRA_JMP_TRUE)
	{
	  ds->trueDest = tree->jumpDest;
	  ds->falseDest = NULL;
	}
      else
	{
	  ds->falseDest = tree->jumpDest;
	  ds->trueDest = NULL;
	}
    }
  else
    ds->trueDest = ds->falseDest = NULL;

  deferred_head = ds;
}

/* Register usage:
 *   R0		scratch
 *   R1		cached address of 1st instance variable
 *   R2		scratch
 *   V0		stack top
 *   V1		cache address of 1st temporary or an outer context
 *		(also) pointer to the inline_cache upon a send
 *   V2		stack pointer
 */


/* Common pieces of code for generating stack operations */

/* Save the old stack top if it was cached in V0 */
#define BEFORE_PUSH(reg) do {						\
  sp_delta += sizeof (PTR);						\
  if (sp_delta > 0) {							\
    jit_stxi(sp_delta, JIT_V2, (reg));					\
  }									\
} while(0)

/* Generate code to evaluate the value to be replaced.  Generate
 * a `pop' by decrementing V2 unless the stack top is cached in V0 -- in
 * this case we can simply overwrite it.
 */
#define BEFORE_SET_TOP do {						\
  if (tree->child) {							\
    emit_code_tree(tree->child);					\
  }									\
  if (sp_delta < 0) {							\
    jit_subi(JIT_V2, JIT_V2, sizeof (PTR));	/* pop stack top */	\
    sp_delta += sizeof (PTR);						\
  }									\
} while(0)

/* Generate code to evaluate the value to be stored, and have it loaded
 * in V0.  */
#define BEFORE_STORE do {						\
  emit_code_tree(tree->child);						\
  if (sp_delta < 0) {							\
    jit_ldr(JIT_V0, JIT_V2);						\
    jit_subi(JIT_V2, JIT_V2, sizeof (PTR));	/* pop stack top */	\
    sp_delta += sizeof (PTR);						\
  }									\
} while(0)


/* Common pieces of code for generating & caching addresses */

#define TEMP_OFS(tree)	   (sizeof (PTR) * (((intptr_t) ((tree)->data)) & 255))
#define REC_VAR_OFS(tree)  offsetof(struct object_s, data[(intptr_t) ((tree)->data)])
#define STACK_OFS(tree)	   (offsetof(struct gst_block_context, contextStack) + \
				TEMP_OFS (tree))


/* Cache the address of the first instance variable in R1 */
#define CACHE_REC_VAR do {						\
  if (!rec_var_cached) {			/* in R1 */		\
    if (!self_cached) {			/* in V0 */			\
      jit_ldi(JIT_R1, &_gst_self);					\
      jit_ldxi(JIT_R1, JIT_R1, offsetof(struct oop_s, object));		\
    } else {								\
      jit_ldxi(JIT_R1, JIT_V0, offsetof(struct oop_s, object));		\
    }									\
    rec_var_cached = true;						\
  }									\
} while(0)

/* Cache the address of the first temporary variable in V1 */
#define CACHE_TEMP do {							\
  if (stack_cached != 0) {		/* in V1 */			\
    jit_ldi(JIT_V1, &_gst_temporaries);					\
    stack_cached = 0;							\
  }									\
} while(0)

#define CACHE_NOTHING do {						\
  rec_var_cached = false;						\
  stack_cached = -1;							\
  self_cached = false;							\
} while(0)

/* Cache into V1 the address of the outer context specified by the
   code_tree.  If the address of another outer context (whose depth is
   lower) is currently cached, avoid walking the list of outer
   contexts from the start.  This optimizes bytecode sequences such as
 
 		push outer variable, n = 1, index = 2
 		store outer variable, n = 2, index = 0
 
   Remember that stack_cached = 0 means `cache the address of the
   first temporary' (the address of the Context's first *indexed*
   instance variable), while stack_cached > 0 means `cache the address
   of the n-th outer context' (i.e. the address of the Context's first
   *fixed* instance variable).  Although confusing, this was done
   because the VM provides the address of the first indexed instance
   variable for thisContext into the `_gst_temporaries' variable.  */
#define CACHE_OUTER_CONTEXT do {					   \
  long scopes;								   \
  scopes = ((long) tree->data) >> 8;					   \
  if (stack_cached <= 0 || stack_cached > scopes) {			   \
    jit_ldi(JIT_V1, &_gst_this_context_oop);				   \
    jit_ldxi(JIT_V1, JIT_V1, offsetof(struct oop_s, object));		   \
    stack_cached = scopes;						   \
  } else {								   \
    scopes -= stack_cached;						   \
    stack_cached += scopes;						   \
  }									   \
  while (scopes--) {							   \
    jit_ldxi(JIT_V1, JIT_V1, offsetof(struct gst_block_context, outerContext)); \
    jit_ldxi(JIT_V1, JIT_V1, offsetof(struct oop_s, object));		   \
  }									   \
} while(0)


/* Miscellaneous pieces of code */

/* Push the children on the stack -- needed for sends */
#define PUSH_CHILDREN do {						\
  code_tree *child;							\
									\
  /* Emit code to generate the receiver and the arguments */		\
  for(child = tree->child; child; child = child->next) {		\
    emit_code_tree(child);						\
  }									\
} while(0)

/* Remember that the stack top is cached in V0, and import V2 (the
 * stack pointer) from the sp variable.  */
#define KEEP_V0_IMPORT_SP do {						\
  jit_ldi(JIT_V2, &sp);							\
  sp_delta = 0;								\
} while(0)

/* Remember that the stack top is *not* cached in V0, and import V2 (the
 * stack pointer) from the sp variable.  */
#define IMPORT_SP do {							\
  jit_ldi(JIT_V2, &sp);							\
  sp_delta = -(int)sizeof (PTR);					\
} while(0)

/* Export V2 (the stack pointer) into the sp variable; the top of the
 * stack is assured to be in *sp, not in V0.  */
#define EXPORT_SP(reg) do {						\
  if (sp_delta >= 0) {							\
    sp_delta += sizeof (PTR);						\
    jit_stxi(sp_delta, JIT_V2, (reg));					\
    jit_addi(JIT_V2, JIT_V2, sp_delta);					\
    jit_sti(&sp, JIT_V2);						\
    sp_delta = -(int)sizeof (PTR);					\
  }									\
} while(0)

/* Export V2 (the stack pointer) into the sp variable; the top of the
 * stack is assured to be in *sp AND in V0.  */
#define CACHE_STACK_TOP do {						\
  if (sp_delta < 0) {							\
    jit_ldr(JIT_V0, JIT_V2);						\
  } else { 								\
    EXPORT_SP (JIT_V0);							\
  }									\
} while(0)

/* Export V2 (the stack pointer) into the sp variable, without pushing
 * the value cached in V0.  */
#define KEEP_V0_EXPORT_SP do {						\
  if (sp_delta < 0) {							\
    jit_ldr(JIT_V0, JIT_V2);						\
  }									\
  if (sp_delta != 0) {							\
    jit_addi(JIT_V2, JIT_V2, sp_delta);					\
  }									\
  jit_sti(&sp, JIT_V2);							\
  sp_delta = -(int)sizeof (PTR);					\
} while(0)

/* Export V2 (the stack pointer) into the sp variable, without
 * saving the old stack top if it was cached in V0.  */
#define POP_EXPORT_SP do {						\
  if (sp_delta) {							\
    jit_addi(JIT_V2, JIT_V2, sp_delta);					\
  }									\
  jit_sti(&sp, JIT_V2);							\
  jit_ldr(JIT_V0, JIT_V2);						\
  sp_delta = -(int)sizeof (PTR);					\
} while(0)

/* Do a conditional jump to tree->jumpDest if the top of the stack
 * is successOOP, or to non_boolean_code if it is anything but failOOP.  */
#define CONDITIONAL_JUMP(successOOP, failOOP) do {			\
  jit_node_t *jump;							\
  									\
  /* Save the value of the top of the stack */				\
  if (sp_delta < 0) { 							\
    jit_ldr(JIT_R0, JIT_V2);						\
  } else {								\
    jit_movr(JIT_R0, JIT_V0);						\
  }									\
  POP_EXPORT_SP;							\
									\
  jump = jit_beqi(JIT_R0, (jit_word_t)successOOP);			\
  jit_patch_at(jump, tree->jumpDest);					\
  jump = jit_beqi(JIT_R0, (jit_word_t)failOOP);				\
  jit_patch_abs(jit_jmpi(), non_boolean_code);				\
  jit_patch(jump);							\
  CACHE_NOTHING;							\
} while(0)



/* Pieces of code for inlining */

/* Don't inline if doing a send to super */
#define DONT_INLINE_SUPER do {						\
  if(ic->is_super) {							\
    gen_send(tree);							\
    return;								\
  }									\
} while(0)

/* Don't attempt to inline an arithmetic operation if one of its
 * argument is known not to be a SmallInteger.
 */
#define DONT_INLINE_NONINTEGER do {					\
  if (NOT_INTEGER(tree->child) || NOT_INTEGER(tree->child->next)) {	\
    gen_send(tree);							\
    return;								\
  }									\
} while(0)

/* Create a `true' or `false' oop if the value is required `as is'; else
 * compile a `jump if true' or `jump if false' native opcode.  This is
 * the equivalent of the `jump lookahead' option in the bytecode interpreter.
 */
#define INLINED_CONDITIONAL do {					\
  jit_node_t *jmp;							\
  									\
  switch (tree->operation & TREE_EXTRA) {				\
    case TREE_EXTRA_NONE:						\
    case TREE_EXTRA_POP:						\
    case TREE_EXTRA_RETURN:						\
    case TREE_EXTRA_METHOD_RET:						\
    case TREE_EXTRA_JMP_ALWAYS:						\
      FALSE_SET(JIT_R0);						\
      jit_lshi(JIT_R0, JIT_R0, LONG_SHIFT+1);				\
      jit_addi(JIT_V0, JIT_R0, (jit_word_t)_gst_true_oop);		\
      break;								\
									\
    case TREE_EXTRA_JMP_TRUE:						\
    case TREE_EXTRA_JMP_FALSE:						\
      if (sp_delta) { 							\
	jit_addi(JIT_V2, JIT_V2, sp_delta);				\
      }									\
      sp_delta = -(int)sizeof (PTR);					\
      if ((tree->operation & TREE_EXTRA) == TREE_EXTRA_JMP_TRUE) {	\
        TRUE_BRANCH(jmp);						\
      } else {								\
        FALSE_BRANCH(jmp);						\
      }									\
      jit_patch_at(jmp, tree->jumpDest);				\
									\
      /* Change the code_tree's operation to TREE_ALREADY_EMITTED */	\
      tree->operation &= TREE_CLASS_CHECKS;				\
      tree->operation |= TREE_NOP | TREE_ALREADY_EMITTED;		\
      break;								\
  }									\
} while(0)


/* Generate code for the only argument, and get the argument in V0.
 * Think twice about it, it is the same as the code needed to compile
 * a store!
 */
#define GET_UNARY_ARG 		BEFORE_STORE

/* Load the two arguments of an inlined binary message, optimizing the
 * common case when the second one is a literal (a == 5, a + 2).
 * reg0 and reg1 will contain the registers in which the arguments have
 * been loaded.
 */
#define GET_BINARY_ARGS do {						\
  code_tree *second = tree->child->next;				\
									\
  emit_code_tree(tree->child);						\
  oop = NULL;								\
  reg0 = JIT_V0;							\
  reg1 = JIT_V1;							\
  if (IS_LITERAL(second)) {						\
    if (sp_delta < 0) {							\
      jit_ldr(JIT_V0, JIT_V2);						\
    }									\
    reg1 = JIT_NOREG;							\
    oop = (OOP) second->data;						\
  } else if (IS_PUSH(second)) { 					\
    if (sp_delta < 0) {							\
      jit_ldr(JIT_V0, JIT_V2);						\
      jit_addi(JIT_V2, JIT_V2, sp_delta);				\
      sp_delta = 0;							\
    }									\
    /* Load the second operand into V1 */				\
    second->operation ^= TREE_PUSH ^ TREE_ALT_PUSH;			\
    emit_code_tree(second);						\
  } else { 								\
    emit_code_tree(second);						\
    if (sp_delta < 0) {							\
      /* We load the 2nd argument and then the 1st */			\
      jit_ldr(JIT_V1, JIT_V2);						\
      jit_ldxi(JIT_V0, JIT_V2, -(int)sizeof (PTR));			\
    } else { 								\
      /* We load the 1st argument; the 2nd is already in V0 */		\
      jit_ldxi(JIT_V1, JIT_V2, sp_delta);				\
      reg0 = JIT_V1;							\
      reg1 = JIT_V0;							\
    }									\
    /* "Pop" the 2nd argument */					\
    sp_delta -= sizeof (PTR);						\
  }									\
  									\
  if (sp_delta) {							\
    jit_addi(JIT_V2, JIT_V2, sp_delta);					\
    sp_delta = 0;							\
  }									\
  CACHE_NOTHING;							\
} while(0)

/* jump out of the instruction flow (to a send whose compilation is
 * deferred to after we compiled the method bytecodes) if one or both
 * arguments are not SmallIntegers.
 */
#define ENSURE_INT_ARGS(isBool, overflow) do {				\
  jit_node_t	*classCheck;						\
									\
  if (IS_INTEGER(tree->child) && IS_INTEGER(tree->child->next)) {	\
    if (isBool || IS_INTEGER(tree)) {					\
      /* No need to do class checks & deferred sends */			\
      overflow = NULL;							\
      break;								\
    }									\
    classCheck = NULL;							\
  } else if (IS_INTEGER(tree->child)) { 				\
    classCheck = jit_bmci(reg1, 1);					\
  } else if (IS_INTEGER(tree->child->next)) {				\
    classCheck = jit_bmci(reg0, 1);					\
  } else { 								\
    jit_andr(JIT_R2, JIT_V0, JIT_V1);					\
    classCheck = jit_bmci(JIT_R2, 1);					\
  }									\
									\
  defer_send(tree, isBool, classCheck, reg0, reg1, oop);		\
  overflow = last_deferred_send();					\
} while(0)

/* These are used to simplify the inlining code, as they group the
 * `second operand is a literal' and `second operand is a register'
 * cases in a single statement.  */
#define EXPAND_(what)		what
#define JMP_IMM_OR_REG(opcode)						\
	((reg1 != JIT_NOREG) 						\
		? EXPAND_(jit_##opcode##r(reg0, reg1))			\
		: EXPAND_(jit_##opcode##i(reg0, (jit_word_t)oop)))
#define CMP_IMM_OR_REG(opcode, reg)					\
	((reg1 != JIT_NOREG) 						\
		? EXPAND_(jit_##opcode##r(reg, reg0, reg1))		\
		: EXPAND_(jit_##opcode##i(reg, reg0, (jit_word_t)oop)))



/* gst_message sends */
void
gen_send (code_tree *tree)
{
  inline_cache *ic = (inline_cache *) tree->data;

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  PUSH_CHILDREN;
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_movi (JIT_V1, (jit_word_t)ic);
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  if (ic->is_super)
    KEEP_V0_EXPORT_SP;
  else
    EXPORT_SP (JIT_V0);
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif

  jit_movi (JIT_R0, tree->bp - bc + tree->bc_len);
  jit_ldxi (JIT_R1, JIT_V1, offsetof (inline_cache, cachedIP));
  jit_sti (&ip, JIT_R0);

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_jmpr (JIT_R1);

  defer_map_patch (&ic->native_ip);
  define_ip_map_entry (tree->bp - bc + tree->bc_len);

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  IMPORT_SP;
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  CACHE_NOTHING;
}

void
gen_binary_int (code_tree *tree)
{
  inline_cache *ic = (inline_cache *) tree->data;
  jit_node_t *overflow;
  int reg0, reg1;
  OOP oop;
  intptr_t imm;
  jit_node_t *jmp;

  DONT_INLINE_SUPER;
  DONT_INLINE_NONINTEGER;
  GET_BINARY_ARGS;
  ENSURE_INT_ARGS (false, overflow);

  imm = (intptr_t) oop;

  /* Now generate the code for the inlined operation.  Don't touch
     reg0/reg1 until we are sure that no overflow happens! */
  switch (ic->imm)
    {
    case PLUS_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      if (reg1 == JIT_NOREG)
	{
	  imm--;		/* strip tag bit */
	  if (imm == 0)
	    {
	      if (reg0 != JIT_V0)
		{
		  jit_movr (JIT_V0, reg0);
		}
	      break;
	    }

	  if (overflow)
	    {
	      jit_movr (JIT_R0, reg0);
	      jmp = jit_boaddi (JIT_R0, imm);
	      jit_patch_at (jmp, overflow);
	      jit_movr (JIT_V0, JIT_R0);
	    }
	  else
	    jit_addi (JIT_V0, reg0, imm);

	}
      else
	{
	  jit_subi (JIT_R0, reg0, 1);	/* remove the tag bit */
	  if (overflow)
	    {
	      jmp = jit_boaddr (JIT_R0, reg1);
	      jit_patch_at (jmp, overflow);
	      jit_movr (JIT_V0, JIT_R0);
	    }
	  else
	    jit_addr (JIT_V0, reg0, reg1);
	}
      break;



    case MINUS_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      if (reg1 == JIT_NOREG)
	{
	  imm--;		/* strip tag bit */
	  if (imm == 0)
	    {
	      if (reg0 != JIT_V0)
		{
		  jit_movr (JIT_V0, reg0);
		}
	      break;
	    }

	  if (overflow)
	    {
	      jit_movr (JIT_R0, reg0);
	      jmp = jit_bosubi (JIT_R0, imm);
	      jit_patch_at (jmp, overflow);
	      jit_movr (JIT_V0, JIT_R0);
	    }
	  else
	    jit_subi (JIT_V0, reg0, imm);

	}
      else
	{
	  if (overflow)
	    {
	      jit_movr (JIT_R0, reg0);
	      jmp = jit_bosubr (JIT_R0, reg1);
	      jit_patch_at (jmp, overflow);
	      jit_addi (JIT_V0, JIT_R0, 1);	/* add back the tag bit 
						 */
	    }
	  else
	    {
	      jit_subr (JIT_V0, reg0, reg1);
	      jit_addi (JIT_V0, JIT_V0, 1);	/* add back the tag bit 
						 */
	    }
	}
      break;



    case TIMES_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
	if (reg1 == JIT_NOREG)
	{
	  jit_node_t *addr1, *addr2;
	  int reduce;

	  imm >>= 1;
	  if (imm == 0)
	    {
	      jit_movi (JIT_V0, (jit_word_t)FROM_INT (0));
	      break;
	    }
	  else if (imm == 1)
	    {
	      if (reg0 != JIT_V0)
		jit_movr (JIT_V0, reg0);
	      break;
	    }
	  else if (imm == -1)
	    {
	      if (overflow)
		{
		  jmp = jit_beqi (reg0, (jit_word_t)FROM_INT (MIN_ST_INT));
		  jit_patch_at (jmp, overflow);
		}
	      jit_rsbi (JIT_V0, reg0, 2);
	      break;
	    }

	  if (overflow)
	    {
	      if (imm < 0)
		{
		  addr1 =
		    jit_blti (reg0,
			      (jit_word_t)FROM_INT (MIN_ST_INT / -imm));
		  addr2 =
		    jit_bgti (reg0,
			      (jit_word_t)FROM_INT (MAX_ST_INT / -imm));
		}
	      else
		{
		  addr1 =
		    jit_blti (reg0,
			      (jit_word_t)FROM_INT (MIN_ST_INT / imm));
		  addr2 =
		    jit_bgti (reg0,
			      (jit_word_t)FROM_INT (MAX_ST_INT / imm));
		}
	      jit_patch_at (addr1, overflow);
	      jit_patch_at (addr2, overflow);
	    }

	  /* Do some strength reduction...  */
	  reduce = analyze_factor (imm);
	  if (reduce == 0)
	    jit_muli (JIT_V0, reg0, imm);

	  else if ((reduce & 0x00FF00) == 0)
	    jit_lshi (JIT_V0, reg0, reduce);

	  else if (reduce & 255)
	    {
	      jit_lshi (JIT_R0, reg0, reduce & 255);
	      jit_lshi (JIT_V0, reg0, reduce >> 8);
	      jit_addr (JIT_V0, JIT_V0, JIT_R0);
	    }
	  else
	    {
	      jit_lshi (JIT_R0, reg0, reduce >> 8);
	      jit_addr (JIT_V0, reg0, JIT_R0);
	    }

	  /* remove the excess due to the tag bit: ((x-1) / 2 * imm) *
	     2 + 1 = x * imm - imm + 1 = (x*imm) - (imm-1) */
	  jit_subi (JIT_V0, reg0, imm - 1);

	}
      else
	{
	  jit_rshi (JIT_R1, reg0, 1);
	  jit_rshi (JIT_R0, reg1, 1);
	  if (overflow)
	    {
	      jit_qmulr (JIT_R2, JIT_R0, JIT_R0, JIT_R1);	/* compute high 
								   bits */

	      /* check for sensible bits of the result in R0, and in
	         bits 30-31 of R2 */
	      jit_rshi (JIT_R1, JIT_R0, sizeof (PTR) * 8 - 1);
	      jmp = jit_bner (JIT_R0, JIT_R1);
	      jit_patch_at (jmp, overflow);

	      jit_xorr (JIT_R1, JIT_R0, JIT_R2);
	      jmp =
		jit_bmsi (JIT_R1, 3UL << (sizeof (PTR) * 8 - 2));
	      jit_patch_at (jmp, overflow);
	    }
	  else
	    jit_mulr (JIT_R2, JIT_R0, JIT_R1);

	  jit_addr (JIT_V0, JIT_R2, JIT_R2);
	  jit_ori (JIT_V0, JIT_V0, 1);
	}
      break;



    case INTEGER_DIVIDE_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      if (reg1 == JIT_NOREG)
	{
	  int shift;
	  mst_Boolean adjust;
	  uintptr_t factor;

	  imm >>= 1;
	  if (imm == 0)
	    {
	      jmp = jit_jmpi ();
	      jit_patch_at (jmp, overflow);
	      break;
	    }
	  else if (imm == 1)
	    {
	      if (reg0 != JIT_V0)
		jit_movr (JIT_V0, reg0);
	      break;
	    }
	  else if (imm == -1)
	    {
	      if (overflow)
		{
		  jmp = jit_beqi (reg0, (jit_word_t)FROM_INT (MIN_ST_INT));
		  jit_patch_at (jmp, overflow);
		}
	      jit_rsbi (JIT_V0, reg0, 2);
	      break;
	    }

	  jit_rshi (reg0, reg0, 1);

	  if (imm < 0)
	    {
	      jit_negr (reg0, reg0);
	      imm = -imm;
	    }

	  /* Fix the sign of the result: reg0 = imm - _gst_self - 1 if
	     reg0 < 0 All these instructions are no-ops if reg0 > 0,
	     because R0=R1=0 */
	  jit_rshi (JIT_R0, reg0, 8 * sizeof (PTR) - 1);
	  jit_andi (JIT_R1, JIT_R0, imm - 1);	/* if reg0 < 0, reg0
						   is...  */
	  jit_subr (reg0, reg0, JIT_R1);	/* _gst_self - imm + 1 */
	  jit_xorr (reg0, reg0, JIT_R0);	/* imm - _gst_self - 2 */
	  jit_subr (reg0, reg0, JIT_R0);	/* imm - _gst_self - 1 */

	  /* Do some strength reduction...  */
	  analyze_dividend (imm, &shift, &adjust, &factor);

	  if (adjust)
	    {
	      /* If adjust is true, we have to sum 1 here, and the
	         carry after the multiplication.  */
	      jit_movi (JIT_R1, 0);
	      jit_addci (reg0, reg0, 1);
	      jit_addxi (JIT_R1, JIT_R1, 0);
	    }

	  shift--;		/* for the tag bit */
	  if (factor)
	    jit_qmuli (JIT_R2, reg0, reg0, factor);

	  if (shift < 0)
	    jit_lshi (reg0, reg0, -shift);
	  else if (shift > 0)
	    jit_rshi (reg0, reg0, shift);

	  if (adjust)
	    jit_subr (reg0, reg0, JIT_R1);

	  /* negate the result if the signs were different */
	  jit_xorr (reg0, reg0, JIT_R0);
	  jit_subr (reg0, reg0, JIT_R0);

	  /* now add the tag bit */
	  jit_ori (JIT_V0, reg0, 1);

	}
      else
	{
	  if (overflow)
	    {
	      jmp = jit_beqi (reg1, (jit_word_t)FROM_INT (0));
	      jit_patch_at (jmp, overflow);
	    }

	  jit_rshi (reg1, reg1, 1);
	  jit_rshi (reg0, reg0, 1);

	  /* Make the divisor positive */
	  jit_rshi (JIT_R0, reg1, 8 * sizeof (PTR) - 1);
	  jit_xorr (reg0, reg0, JIT_R0);
	  jit_xorr (reg1, reg1, JIT_R0);
	  jit_subr (reg0, reg0, JIT_R0);
	  jit_subr (reg1, reg1, JIT_R0);

	  /* Fix the result if signs differ: reg0 -= reg1-1 */
	  jit_rshi (JIT_R1, reg0, 8 * sizeof (PTR) - 1);
	  jit_subi (JIT_R0, reg1, 1);
	  jit_andr (JIT_R0, JIT_R0, JIT_R1);	/* if reg0 < 0, reg0
						   is...  */
	  jit_subr (reg0, reg0, JIT_R0);	/* _gst_self - imm + 1 */
	  jit_xorr (reg0, reg0, JIT_R1);	/* imm - _gst_self - 2 */
	  jit_subr (reg0, reg0, JIT_R1);	/* imm - _gst_self - 1 */

	  /* divide, then negate the result if the signs were different 
	   */
	  jit_divr (JIT_R0, reg0, reg1);
	  jit_xorr (JIT_R0, JIT_R0, JIT_R1);
	  jit_subr (JIT_R0, JIT_R0, JIT_R1);

	  /* add the tag bit */
	  jit_addr (JIT_V0, JIT_R0, JIT_R0);
	  jit_ori (JIT_V0, JIT_V0, 1);
	}
      break;



    case REMAINDER_SPECIAL:
    case BIT_SHIFT_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      /* not yet */
      jmp = jit_jmpi ();
      jit_patch_at (jmp, overflow);
      break;

    case BIT_AND_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      CMP_IMM_OR_REG (and, JIT_V0);
      break;
    case BIT_OR_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      CMP_IMM_OR_REG (or, JIT_V0);
      break;

    case BIT_XOR_SPECIAL:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      /* For XOR, the tag bits of the two operands cancel (unlike
	 AND and OR), so we cannot simply use the CMP_IMM_OR_REG macro.  */
      if (reg1 != JIT_NOREG)
	{
	  jit_xorr(JIT_V0, reg0, reg1);
	  jit_addi(JIT_V0, JIT_V0, 1);	/* Put back the tag bit.  */
	}
      else
	{
	  imm--;				/* Strip the tag bit.  */
	  jit_xori(JIT_V0, reg0, imm);
	}

      break;
    }
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif

  EXPORT_SP (JIT_V0);
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  if (overflow)
    finish_deferred_send ();
}

void
gen_binary_bool (code_tree *tree)
{
  inline_cache *ic = (inline_cache *) tree->data;
  jit_node_t *deferredSend;
  int reg0, reg1;
  OOP oop;

  DONT_INLINE_SUPER;
  if (ic->imm != SAME_OBJECT_SPECIAL)
    DONT_INLINE_NONINTEGER;

  GET_BINARY_ARGS;
  if (ic->imm != SAME_OBJECT_SPECIAL)
    ENSURE_INT_ARGS (true, deferredSend);

  else
    deferredSend = NULL;

#define TRUE_BRANCH(jmp)						\
  switch(ic->imm) {							\
    case LESS_THAN_SPECIAL:	jmp = JMP_IMM_OR_REG(blt); break;	\
    case GREATER_THAN_SPECIAL:	jmp = JMP_IMM_OR_REG(bgt); break;	\
    case LESS_EQUAL_SPECIAL:	jmp = JMP_IMM_OR_REG(ble); break;	\
    case GREATER_EQUAL_SPECIAL:	jmp = JMP_IMM_OR_REG(bge); break;	\
    case SAME_OBJECT_SPECIAL:						\
    case EQUAL_SPECIAL:		jmp = JMP_IMM_OR_REG(beq); break;	\
    case NOT_EQUAL_SPECIAL:	jmp = JMP_IMM_OR_REG(bne); break;	\
  }

#define FALSE_BRANCH(jmp)						\
  switch(ic->imm) {							\
    case LESS_THAN_SPECIAL:	jmp = JMP_IMM_OR_REG(bge); break;	\
    case GREATER_THAN_SPECIAL:	jmp = JMP_IMM_OR_REG(ble); break;	\
    case LESS_EQUAL_SPECIAL:	jmp = JMP_IMM_OR_REG(bgt); break;	\
    case GREATER_EQUAL_SPECIAL:	jmp = JMP_IMM_OR_REG(blt); break;	\
    case SAME_OBJECT_SPECIAL:						\
    case EQUAL_SPECIAL:		jmp = JMP_IMM_OR_REG(bne); break;	\
    case NOT_EQUAL_SPECIAL:	jmp = JMP_IMM_OR_REG(beq); break;	\
  }

#define FALSE_SET(reg)							\
  switch(ic->imm) {							\
    case LESS_THAN_SPECIAL:	CMP_IMM_OR_REG(ge, reg); break;		\
    case GREATER_THAN_SPECIAL:	CMP_IMM_OR_REG(le, reg); break;		\
    case LESS_EQUAL_SPECIAL:	CMP_IMM_OR_REG(gt, reg); break;		\
    case GREATER_EQUAL_SPECIAL:	CMP_IMM_OR_REG(lt, reg); break;		\
    case SAME_OBJECT_SPECIAL:						\
    case EQUAL_SPECIAL:		CMP_IMM_OR_REG(ne, reg); break;		\
    case NOT_EQUAL_SPECIAL:	CMP_IMM_OR_REG(eq, reg); break;		\
  }

  INLINED_CONDITIONAL;
#undef TRUE_BRANCH
#undef FALSE_BRANCH
#undef FALSE_SET

  EXPORT_SP (JIT_V0);
  if (deferredSend)
    finish_deferred_send ();
}

void
gen_send_store_lit_var (code_tree *tree)
{
  inline_cache *ic = (inline_cache *) tree->data;
  jit_node_t *overflow;
  int reg0, reg1;
  OOP oop;
  intptr_t imm;
  jit_node_t *addr;

  /* tree->child = value
     tree->child->next = var.  */
  BEFORE_STORE;
  emit_code_tree(tree->child->next);
  BEFORE_PUSH (JIT_V1);
  EXPORT_SP (JIT_V0);
  gen_send (tree);
}

void
gen_dirty_block (code_tree *tree)
{
  GET_UNARY_ARG;

  KEEP_V0_EXPORT_SP;
  jit_prepare ();
  jit_pushargr (JIT_V0);
  jit_finishi (_gst_make_block_closure);
  jit_retval (JIT_V0);

  KEEP_V0_IMPORT_SP;
  CACHE_NOTHING;
}

#if 0
void
gen_fetch_class (code_tree *tree)
{
  inline_cache *ic = (inline_cache *) tree->data;

  DONT_INLINE_SUPER;
  GET_UNARY_ARG;

  if (IS_INTEGER (tree->child))
    jit_movi (JIT_V0, _gst_small_integer_class);

  else if (NOT_INTEGER (tree->child))
    {
      jit_ldxi (JIT_V0, JIT_V0, offsetof (struct oop_s, object));
      jit_ldxi (JIT_V0, JIT_V0, offsetof (struct object_s, objClass));
    }
  else
    {
      jit_node_t *jmp;
      jit_movi (JIT_R0, _gst_small_integer_class);
      jmp = jit_bmsi (JIT_V0, 1);
      jit_ldxi (JIT_R0, JIT_V0, offsetof (struct oop_s, object));
      jit_ldxi (JIT_R0, JIT_R0, offsetof (struct object_s, objClass));
      jit_patch (jmp);
      jit_movr (JIT_V0, JIT_R0);
    }

  self_cached = false;
}
#endif

void
gen_unary_special (code_tree *tree)
{
  inline_cache *ic = (inline_cache *) tree->data;
  jit_node_t *ok1 = NULL, *bad1, *ok2, *ok3;
  int sz;

  DONT_INLINE_SUPER;

  switch (ic->imm)
    {
    case JAVA_AS_INT_SPECIAL:
    case JAVA_AS_LONG_SPECIAL:
      emit_code_tree(tree->child);
      if (IS_INTEGER (tree->child))
	break;

      /* In order to prepare for emitting the send,
	 we have to export the top of the stack here.
	 We won't emit anything in gen_send, though.  */
      CACHE_STACK_TOP;

      if (!NOT_INTEGER (tree->child))
	ok1 = jit_bmsi (JIT_V0, 1);

      sz = (ic->imm == JAVA_AS_LONG_SPECIAL) ? 8 : 4;

      jit_ldr (JIT_R2, JIT_V0);

      /* Check if it belongs to the wrong class...  */
      jit_ldxi (JIT_R0, JIT_R2, offsetof (struct object_s, objClass));
      jit_subi (JIT_R0, JIT_R0, (jit_word_t)_gst_large_positive_integer_class);
      ok2 = jit_beqi (JIT_R0, 0);
      bad1 = jit_bnei (JIT_R0,
			 ((char *) _gst_large_negative_integer_class) -
			 ((char *) _gst_large_positive_integer_class));

      /* Look for too big an integer...  */
      jit_patch (ok2);
      if (SIZEOF_OOP > sizeof (long))
	{
          emit_basic_size_in_r0 (_gst_large_positive_integer_class, false, JIT_R2);
          ok3 = jit_blei_u (JIT_R0, sz);
	}
      else
	{
           /* We can check the size field directly.  */
          jit_ldxi (JIT_R0, JIT_R2, offsetof (struct object_s, objSize));
          ok3 = jit_blei (JIT_R0,
			  (jit_word_t)FROM_INT (OBJ_HEADER_SIZE_WORDS + sz / SIZEOF_OOP));
	}

      jit_patch (bad1);
      gen_send (tree);
      jit_patch (ok3);
      if (ok1)
	jit_patch (ok1);
      self_cached = false;
      return;

    default:
      GET_UNARY_ARG;
      abort ();
    }
}

void
gen_unary_bool (code_tree *tree)
{
  inline_cache *ic = (inline_cache *) tree->data;
  mst_Boolean compileIsNil = ic->imm == IS_NIL_SPECIAL;

  DONT_INLINE_SUPER;
  GET_UNARY_ARG;

#define TRUE_BRANCH(jmp)     jmp = compileIsNil ? jit_beqi(JIT_V0, (jit_word_t)_gst_nil_oop) \
					        : jit_bnei(JIT_V0, (jit_word_t)_gst_nil_oop)
#define FALSE_BRANCH(jmp)    jmp = compileIsNil ? jit_bnei(JIT_V0, (jit_word_t)_gst_nil_oop) \
						: jit_beqi(JIT_V0, (jit_word_t)_gst_nil_oop)
#define FALSE_SET(reg)		   compileIsNil ? jit_nei ((reg),  JIT_V0, (jit_word_t)_gst_nil_oop) \
						: jit_eqi ((reg),  JIT_V0, (jit_word_t)_gst_nil_oop)
  INLINED_CONDITIONAL;
#undef TRUE_BRANCH
#undef FALSE_BRANCH
#undef FALSE_SET
}

void
gen_pop_into_array (code_tree *tree)
{
  mst_Boolean useCachedR0;
  code_tree *array, *value;
  long index;

  array = tree->child;
  value = array->next;
  index = (long) tree->data;
  useCachedR0 = (array->operation & (TREE_OP | TREE_SUBOP))
    == (TREE_STORE | TREE_POP_INTO_ARRAY);

  /* This code is the same as GET_BINARY_ARGS, but it forces the first 
     parameter in V0 and the second in V1. This is because the bytecode 
     leaves the first parameter in the stack top */

  emit_code_tree (array);
  if (IS_PUSH (value))
    {
      if (sp_delta < 0)
	{
	  jit_ldr (JIT_V0, JIT_V2);
	  jit_addi (JIT_V2, JIT_V2, sp_delta);
	  sp_delta = 0;
	}
      /* Load the value operand into V1 */
      value->operation ^= TREE_PUSH ^ TREE_ALT_PUSH;
      emit_code_tree (value);
    }
  else
    {
      emit_code_tree (value);
      if (sp_delta < 0)
	{
	  /* We load the 2nd argument and then the 1st */
	  jit_ldr (JIT_V1, JIT_V2);
	  jit_ldxi (JIT_V0, JIT_V2, -(int)sizeof (PTR));
	}
      else
	{
	  /* The 2nd argument is already in V0, move it in V1 */
	  jit_movr (JIT_V1, JIT_V0);
	  jit_ldxi (JIT_V0, JIT_V2, sp_delta);
	}

      /* "Pop" the 2nd argument */
      sp_delta -= sizeof (PTR);
      useCachedR0 = false;
    }

  if (sp_delta)
    {
      jit_addi (JIT_V2, JIT_V2, sp_delta);
      sp_delta = 0;
    }

  if (!useCachedR0)
    {
      /* Dereference the OOP into R0 */
      jit_ldxi (JIT_R0, JIT_V0, offsetof (struct oop_s, object));
    }

  jit_stxi (offsetof (struct object_s, data) + index * sizeof (OOP), JIT_R0, JIT_V1);
}


/* Stores */
void
gen_store_rec_var (code_tree *tree)
{
  BEFORE_STORE;
  CACHE_REC_VAR;

  jit_stxi (REC_VAR_OFS (tree), JIT_R1, JIT_V0);
}

void
gen_store_temp (code_tree *tree)
{
  BEFORE_STORE;
  CACHE_TEMP;

  jit_stxi (TEMP_OFS (tree), JIT_V1, JIT_V0);
}

void
gen_store_lit_var (code_tree *tree)
{
  char *assocOOP = ((char *) tree->data) + offsetof (struct oop_s, object);
  BEFORE_STORE;

  jit_ldi (JIT_R0, assocOOP);
  jit_stxi (offsetof (struct gst_association, value), JIT_R0, JIT_V0);
}

void
gen_store_outer (code_tree *tree)
{
  BEFORE_STORE;
  CACHE_OUTER_CONTEXT;

  jit_stxi (STACK_OFS (tree), JIT_V1, JIT_V0);
}

/* Pushes */
void
gen_push_rec_var (code_tree *tree)
{
  BEFORE_PUSH (JIT_V0);
  CACHE_REC_VAR;

  jit_ldxi (JIT_V0, JIT_R1, REC_VAR_OFS (tree));
  self_cached = false;
}

void
gen_push_temp (code_tree *tree)
{
  BEFORE_PUSH (JIT_V0);
  CACHE_TEMP;

  jit_ldxi (JIT_V0, JIT_V1, TEMP_OFS (tree));
  self_cached = false;
}

void
gen_push_lit_const (code_tree *tree)
{
  BEFORE_PUSH (JIT_V0);

  jit_movi (JIT_V0, (jit_word_t)tree->data);
  self_cached = false;
}

void
gen_push_lit_var (code_tree *tree)
{
  char *assocOOP = ((char *) tree->data) + offsetof (struct oop_s, object);
  BEFORE_PUSH (JIT_V0);

  jit_ldi (JIT_V0, assocOOP);
  jit_ldxi (JIT_V0, JIT_V0, offsetof (struct gst_association, value));
  self_cached = false;
}

void
gen_dup_top (code_tree *tree)
{
  if (sp_delta < 0)
    jit_ldr (JIT_V0, JIT_V2);

  BEFORE_PUSH (JIT_V0);
}

void
gen_push_self (code_tree *tree)
{
  BEFORE_PUSH (JIT_V0);

  if (!self_cached)
    jit_ldi (JIT_V0, &_gst_self);

  self_cached = true;
}

void
gen_push_outer (code_tree *tree)
{
  BEFORE_PUSH (JIT_V0);
  CACHE_OUTER_CONTEXT;

  jit_ldxi (JIT_V0, JIT_V1, STACK_OFS (tree));
  self_cached = false;
}

/* Moves to V1 (alternative push) */
void
gen_alt_rec_var (code_tree *tree)
{
  CACHE_REC_VAR;

  jit_ldxi (JIT_V1, JIT_R1, REC_VAR_OFS (tree));
  stack_cached = -1;
}

void
gen_alt_temp (code_tree *tree)
{
  CACHE_TEMP;

  jit_ldxi (JIT_V1, JIT_V1, TEMP_OFS (tree));
  stack_cached = -1;
}

void
gen_alt_lit_const (code_tree *tree)
{
  jit_movi (JIT_V1, (jit_word_t)tree->data);
  stack_cached = -1;
}

void
gen_alt_lit_var (code_tree *tree)
{
  char *assocOOP = ((char *) tree->data) + offsetof (struct oop_s, object);

  jit_ldi (JIT_V1, assocOOP);
  jit_ldxi (JIT_V1, JIT_V1, offsetof (struct gst_association, value));
  stack_cached = -1;
}

void
gen_get_top (code_tree *tree)
{
  if (sp_delta < 0)
    jit_ldr (JIT_V1, JIT_V2);

  else
    jit_movr (JIT_V1, JIT_V0);

  stack_cached = -1;
}

void
gen_alt_self (code_tree *tree)
{
  if (!self_cached)
    jit_ldi (JIT_V1, &_gst_self);

  else
    jit_movr (JIT_V1, JIT_V0);

  stack_cached = -1;
}

void
gen_alt_outer (code_tree *tree)
{
  CACHE_OUTER_CONTEXT;

  jit_ldxi (JIT_V1, JIT_V1, STACK_OFS (tree));
  stack_cached = -1;
}

/* Set top */
void
gen_top_rec_var (code_tree *tree)
{
  BEFORE_SET_TOP;
  CACHE_REC_VAR;

  jit_ldxi (JIT_V0, JIT_R1, REC_VAR_OFS (tree));
  self_cached = false;
}

void
gen_top_temp (code_tree *tree)
{
  BEFORE_SET_TOP;
  CACHE_TEMP;

  jit_ldxi (JIT_V0, JIT_V1, TEMP_OFS (tree));
  self_cached = false;
}

void
gen_top_self (code_tree *tree)
{
  BEFORE_SET_TOP;

  if (!self_cached)
    jit_ldi (JIT_V0, &_gst_self);

  self_cached = true;
}

void
gen_top_outer (code_tree *tree)
{
  int index;
  BEFORE_SET_TOP;
  CACHE_OUTER_CONTEXT;
  index = ((gst_uchar *) tree->data)[0];

  jit_ldxi (JIT_V0, JIT_V1, STACK_OFS (tree));
  self_cached = false;
}

void
gen_top_lit_const (code_tree *tree)
{
  BEFORE_SET_TOP;

  jit_movi (JIT_V0, (jit_word_t)tree->data);
  self_cached = false;
}

void
gen_top_lit_var (code_tree *tree)
{
  char *assocOOP = ((char *) tree->data) + offsetof (struct oop_s, object);

  BEFORE_SET_TOP;

  jit_ldi (JIT_V0, assocOOP);
  jit_ldxi (JIT_V0, JIT_V0, offsetof (struct gst_association, value));
  self_cached = false;
}

void
gen_invalid (code_tree *tree)
{
  printf ("Invalid operation %o in the code tree", tree->operation);
  abort ();
}

void
gen_nothing (code_tree *tree)
{
}

void
gen_two_extras (code_tree *tree)
{
  /* Emit code for the real node and the first extra;
     emit_code_tree will take care of the second extra
     held by TREE.  */
  emit_code_tree (tree->next);	/* emit the code for the real node */
}

void
emit_code_tree (code_tree *tree)
{
  int operation;
  jit_node_t *jmp;

  operation = tree->operation & (TREE_OP | TREE_SUBOP);
  emit_operation_funcs[operation] (tree);

  /* Now emit the extras.  */
  switch (tree->operation & TREE_EXTRA)
    {
    case TREE_EXTRA_NONE:
      break;

    case TREE_EXTRA_POP:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      POP_EXPORT_SP;
      break;

    case TREE_EXTRA_RETURN:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      CACHE_STACK_TOP;
      jit_calli (PTR_UNWIND_CONTEXT);
      IMPORT_SP;
      jit_ldi (JIT_R0, &native_ip);
      jit_str (JIT_V2, JIT_V0);
      jit_jmpr (JIT_R0);
      break;

    case TREE_EXTRA_METHOD_RET:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      CACHE_STACK_TOP;
      jit_calli (PTR_UNWIND_METHOD);
      jit_retval (JIT_R0);
      jmp = jit_bnei (JIT_R0, false);
      jit_patch_abs (jit_jmpi (), bad_return_code);
      jit_patch (jmp);
      IMPORT_SP;
      jit_ldi (JIT_R0, &native_ip);
      jit_str (JIT_V2, JIT_V0);
      jit_jmpr (JIT_R0);
      break;

    case TREE_EXTRA_JMP_ALWAYS:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      CACHE_STACK_TOP;
      jmp = jit_jmpi ();
      jit_patch_at (jmp, tree->jumpDest);
      break;

    case TREE_EXTRA_JMP_TRUE:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      CONDITIONAL_JUMP (_gst_true_oop, _gst_false_oop);
      break;

    case TREE_EXTRA_JMP_FALSE:
#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
      CONDITIONAL_JUMP (_gst_false_oop, _gst_true_oop);
      break;
    }

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  /* Change the code_tree's operation field to TREE_ALREADY_EMITTED,
     and null the extra op. field */
  tree->operation &= TREE_CLASS_CHECKS;
  tree->operation |= TREE_NOP | TREE_ALREADY_EMITTED;
}



/* Initialization and other code generation (prologs, interrupt checks) */

void
emit_deferred_sends (deferred_send *ds)
{
  jit_node_t *jmp;
  code_tree *tree;
  inline_cache *ic;

  if (!ds)
    return;

  emit_deferred_sends (ds->next);

  tree = ds->tree;
  ic = (inline_cache *) tree->data;
  assert (!ic->is_super);

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_link (ds->address);
  if (ds->reg1 == JIT_NOREG)
    {
      jit_movi (JIT_R0, (jit_word_t)ds->oop);
      ds->reg1 = JIT_R0;
    }

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_stxi (sizeof (PTR) * 1, JIT_V2, ds->reg0);
  jit_stxi (sizeof (PTR) * 2, JIT_V2, ds->reg1);
  jit_addi (JIT_V2, JIT_V2, sizeof (PTR) * 2);

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_movi (JIT_V1, (jit_word_t)ic);
  jit_sti (&sp, JIT_V2);
  jit_movi (JIT_V0, (jit_word_t)(tree->bp - bc));
  jit_ldxi (JIT_R1, JIT_V1, offsetof (inline_cache, cachedIP));
  jit_sti (&ip, JIT_V0);

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_jmpr (JIT_R1);

  defer_map_patch (&ic->native_ip);
  define_ip_map_entry (tree->bp - bc);

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  IMPORT_SP;
  if (ds->trueDest == ds->falseDest)
    {
      /* This was an arithmetic deferred send.  */
      jit_ldr (JIT_V0, JIT_V2);
      jmp = jit_jmpi ();
      jit_patch_at (jmp, ds->trueDest);

    }
  else
    {
      /* This was a boolean deferred send.  */
      jit_ldr (JIT_R0, JIT_V2);
      jit_subi (JIT_V2, JIT_V2, sizeof (PTR));
      jit_ldr (JIT_V0, JIT_V2);

      jmp = jit_beqi (JIT_R0, (jit_word_t)_gst_true_oop);
      jit_patch_at (jmp, ds->trueDest);

      jmp = jit_beqi (JIT_R0, (jit_word_t)_gst_false_oop);
      jit_patch_at (jmp, ds->falseDest);
      jit_patch_abs (jit_jmpi (), non_boolean_code);
    }
}

void
emit_interrupt_check (int restartReg, int ipOffset)
{
  jit_node_t *jmp, *restart = NULL;

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_ldi_i (JIT_R2, &_gst_except_flag);
  jmp = jit_beqi (JIT_R2, 0);

  /* Save the global ip pointer */
  if (ipOffset != -1)
    {
      jit_movi (JIT_R2, ipOffset);
      jit_sti (&ip, JIT_R2);
    }

  /* Where to restart?*/
  if (restartReg == JIT_NOREG)
      restart = jit_movi (JIT_R0, 0);
  else
      jit_movr (JIT_R0, restartReg);

#if DISASSEMBLE
      jit_note(method_name, __LINE__);
#endif
  jit_patch_abs (jit_jmpi (), _gst_return_from_native_code);
  if (restart)
    jit_patch (restart);
  jit_patch (jmp);
}

/* Auxiliary function for inlined primitives.  Retrieves the receiver's
 * basicSize in R0, expects the pointer to the object data in objectReg.
 * Destroys V1.  */
void
emit_basic_size_in_r0 (OOP classOOP, mst_Boolean tagged, int objectReg)
{
  int adjust;

  int shape = CLASS_INSTANCE_SPEC (classOOP) & ISP_INDEXEDVARS;

  if (!CLASS_IS_INDEXABLE (classOOP))
    {
      jit_movi (JIT_R0, (jit_word_t)FROM_INT (0));
      return;
    }

  /* Not yet implemented.  */
  if (shape != GST_ISP_POINTER
      && shape != GST_ISP_SCHAR
      && shape != GST_ISP_CHARACTER
      && shape != GST_ISP_UCHAR)
    abort ();

  adjust = CLASS_FIXED_FIELDS (classOOP) +
    sizeof (gst_object_header) / sizeof (PTR);

  if (objectReg == JIT_NOREG)
    {
      jit_ldxi (JIT_R2, JIT_V0, offsetof (struct oop_s, object));
      objectReg = JIT_R2;
    }

  jit_ldxi (JIT_R0, objectReg, offsetof (struct object_s, objSize));

  if (shape != GST_ISP_POINTER)
    jit_ldxi (JIT_V1, JIT_V0, offsetof (struct oop_s, flags));

  if (!tagged)
    /* Remove the tag bit */
    jit_rshi (JIT_R0, JIT_R0, 1);
  else
    adjust = adjust * 2;

  if (shape != GST_ISP_POINTER)
    {
      jit_andi (JIT_V1, JIT_V1, EMPTY_BYTES);
      jit_lshi (JIT_R0, JIT_R0, LONG_SHIFT);
      jit_subr (JIT_R0, JIT_R0, JIT_V1);

      adjust *= sizeof (PTR);
      if (tagged)
        {
          jit_subr (JIT_R0, JIT_R0, JIT_V1);

          /* Move the tag bit back to bit 0 after the long shift above */
          adjust += sizeof (PTR) - 1;
        }
    }

  if (adjust)
    jit_subi (JIT_R0, JIT_R0, adjust);
}

/* This takes care of emitting the code for inlined primitives.
   Returns a new set of attributes which applies to the inlined code.  */
mst_Boolean
emit_inlined_primitive (int primitive, int numArgs, int attr)
{
  switch (primitive)
    {
    case 60:
      {
	jit_node_t *fail1, *fail2;
	OOP charBase = CHAR_OOP_AT (0);
	int numFixed = CLASS_FIXED_FIELDS (current->receiverClass) +
	  sizeof (gst_object_header) / sizeof (PTR);

	int shape = CLASS_INSTANCE_SPEC (current->receiverClass) & ISP_INDEXEDVARS;

	if (numArgs != 1)
	  break;

	if (!shape)
	  {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    /* return failure */
	    jit_movi (JIT_R0, -1);
	    return PRIM_FAIL | PRIM_INLINED;
	  }

	else if (shape != GST_ISP_POINTER && shape != GST_ISP_UCHAR
		 && shape != GST_ISP_SCHAR && shape != GST_ISP_CHARACTER)
	  /* too complicated to return LargeIntegers */
	  break;

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	jit_ldi (JIT_R1, &sp);
	emit_basic_size_in_r0 (current->receiverClass, false, JIT_NOREG);

	/* Point R2 to the first indexed slot */
	jit_addi (JIT_R2, JIT_R2, numFixed * sizeof (PTR));

	/* Load the index and test it: remove tag bit, then check if
	   (unsigned) (V1 - 1) >= R0 */

	jit_ldr (JIT_V1, JIT_R1);
	fail1 = jit_bmci (JIT_V1, 1);

	jit_rshi_u (JIT_V1, JIT_V1, 1);
	jit_subi (JIT_V1, JIT_V1, 1);
	fail2 = jit_bger_u (JIT_V1, JIT_R0);

	/* adjust stack top */
	jit_subi (JIT_R1, JIT_R1, sizeof (PTR));

	/* Now R2 + V1 << SOMETHING contains the pointer to the slot
	   (SOMETHING depends on the shape).  */
	switch (shape)
	  {
	  case GST_ISP_POINTER:
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    jit_lshi (JIT_V1, JIT_V1, LONG_SHIFT);
	    jit_ldxr (JIT_R0, JIT_R2, JIT_V1);
	    break;

	  case GST_ISP_UCHAR:
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    jit_ldxr_uc (JIT_R0, JIT_R2, JIT_V1);

	    /* Tag the byte we read */
	    jit_addr (JIT_R0, JIT_R0, JIT_R0);
	    jit_addi (JIT_R0, JIT_R0, 1);
	    break;

	  case GST_ISP_SCHAR:
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    jit_ldxr_c (JIT_R0, JIT_R2, JIT_V1);

	    /* Tag the byte we read */
	    jit_addr (JIT_R0, JIT_R0, JIT_R0);
	    jit_addi (JIT_R0, JIT_R0, 1);
	    break;

	  case GST_ISP_CHARACTER:
	    {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	      jit_ldxr_uc (JIT_R0, JIT_R2, JIT_V1);

              /* Convert to a character */
              jit_lshi (JIT_R0, JIT_R0, LONG_SHIFT + 1);
              jit_addi (JIT_R0, JIT_R0, (jit_word_t)charBase);
	    }
	  }

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	/* Store the result and the new stack pointer */
	jit_str (JIT_R1, JIT_R0);
	jit_sti (&sp, JIT_R1);

	jit_movi (JIT_R0, -1);

	jit_patch (fail1);
	jit_patch (fail2);

	/* We get here with the _gst_basic_size in R0 upon failure,
	   with -1 upon success.  We need to get 0 upon success and -1
	   upon failure.  */
	jit_rshi (JIT_R0, JIT_R0, 8 * sizeof (PTR) - 1);
	jit_comr (JIT_R0, JIT_R0);

	return PRIM_FAIL | PRIM_SUCCEED | PRIM_INLINED;
      }
      break;

    case 61:
      {
	jit_node_t *fail0, *fail1, *fail2, *fail3, *fail4;
	OOP charBase = CHAR_OOP_AT (0);
	int numFixed = CLASS_FIXED_FIELDS (current->receiverClass) +
	  sizeof (gst_object_header) / sizeof (PTR);

	int shape = CLASS_INSTANCE_SPEC (current->receiverClass) & ISP_INDEXEDVARS;

	if (numArgs != 2)
	  break;

	if (!shape)
	  {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    /* return failure */
	    jit_movi (JIT_R0, -1);
	    return PRIM_FAIL | PRIM_INLINED;
	  }

	if (shape != GST_ISP_UCHAR && shape != GST_ISP_POINTER)
	  /* too complicated to convert LargeIntegers */
	  break;

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	jit_ldxi (JIT_V1, JIT_V0, offsetof (struct oop_s, flags));
	fail0 = jit_bmsi (JIT_V1, (jit_word_t)F_READONLY);

	jit_ldi (JIT_R1, &sp);
	emit_basic_size_in_r0 (current->receiverClass, false, JIT_NOREG);

	/* Point R2 to the first indexed slot */
	jit_addi (JIT_R2, JIT_R2, numFixed * sizeof (PTR));

	/* Load the index and test it: remove tag bit, then check if
	   (unsigned) (V1 - 1) >= R0 */

	jit_ldxi (JIT_V1, JIT_R1, -(int)sizeof (PTR));

	fail1 = jit_bmci (JIT_V1, 1);

	jit_rshi_u (JIT_V1, JIT_V1, 1);
	jit_subi (JIT_V1, JIT_V1, 1);
	fail2 = jit_bger_u (JIT_V1, JIT_R0);

	if (shape == GST_ISP_POINTER)
	  jit_lshi (JIT_V1, JIT_V1, LONG_SHIFT);

	/* Compute the effective address to free V1 for the operand */
	jit_addr (JIT_R2, JIT_R2, JIT_V1);
	jit_ldr (JIT_V1, JIT_R1);

	switch (shape)
	  {
	  case GST_ISP_UCHAR:
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    /* Check and untag the byte we store */
	    fail3 = jit_bmci (JIT_V1, 1);
	    jit_rshi_u (JIT_R0, JIT_V1, 1);
	    fail4 = jit_bmsi (JIT_R0, ~255);

	    jit_str_c (JIT_R2, JIT_R0);
	    break;

	  case GST_ISP_CHARACTER:
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    /* Check the character we store */
	    fail3 = jit_bmsi (JIT_V1, 1);

	    jit_subi (JIT_R0, JIT_V1, (jit_word_t)charBase);
	    jit_rshi_u (JIT_R0, JIT_R0, LONG_SHIFT + 1);
	    fail4 = jit_bmsi (JIT_R0, ~255);

	    jit_str_c (JIT_R2, JIT_R0);
	    break;

	  case GST_ISP_POINTER:
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    fail3 = fail4 = NULL;
	    jit_str (JIT_R2, JIT_V1);
	  }

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	/* Store the result and the new stack pointer */
	jit_subi (JIT_R1, JIT_R1, sizeof (PTR) * 2);
	jit_str (JIT_R1, JIT_V1);
	jit_sti (&sp, JIT_R1);

	jit_movi (JIT_R0, -1);

	jit_patch (fail0);
	jit_patch (fail1);
	jit_patch (fail2);
	if (fail3)
	  {
	    jit_patch (fail3);
	    jit_patch (fail4);
	  }

	/* We get here with the _gst_basic_size in R0 upon failure,
	   with -1 upon success.  We need to get 0 upon success and -1
	   upon failure.  */
	jit_rshi (JIT_R0, JIT_R0, 8 * sizeof (PTR) - 1);
	jit_comr (JIT_R0, JIT_R0);

	return PRIM_FAIL | PRIM_SUCCEED | PRIM_INLINED;
      }
      break;

    case 62:
      {
	int shape = CLASS_INSTANCE_SPEC (current->receiverClass) & ISP_INDEXEDVARS;

        if (numArgs != 0)
	  break;

	if (shape != 0 && shape != GST_ISP_UCHAR && shape != GST_ISP_POINTER)
	  /* too complicated to convert LargeIntegers */
	  break;

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
        jit_ldi (JIT_R1, &sp);
        emit_basic_size_in_r0 (current->receiverClass, true, JIT_NOREG);
        jit_str (JIT_R1, JIT_R0);
        return (PRIM_SUCCEED | PRIM_INLINED);
      }

#if 0
    case 70:
      {
	OOP class_oop;
	if (numArgs != 0)
	  break;

	if (!is_a_kind_of (current->receiverClass, _gst_class_class))
	  break;

	class_oop = METACLASS_INSTANCE (current->receiverClass);
	if (CLASS_IS_INDEXABLE (class_oop))
	  {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    /* return failure */
	    jit_movi (JIT_R0, -1);
	    return PRIM_FAIL | PRIM_INLINED;
	  }

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	/* SET_STACKTOP (alloc_oop (instantiate (_gst_self))) */
	jit_prepare ();
	jit_pushargr (JIT_V0);
	jit_finishi (instantiate);
	jit_retval (JIT_R0);

	jit_prepare ();
	jit_pushargr (JIT_R0);
	jit_finishi (alloc_oop);

	jit_retval (JIT_R0);
	jit_ldi (JIT_V1, &sp);
	jit_str (JIT_V1, JIT_R0);
	return (PRIM_SUCCEED | PRIM_INLINED);
      }


    case 71:
      {
	OOP class_oop;
	jit_node_t *fail1, *fail2;

	if (numArgs != 1)
	  break;

	if (!is_a_kind_of (current->receiverClass, _gst_class_class))
	  break;

	class_oop = METACLASS_INSTANCE (current->receiverClass);
	if (!CLASS_IS_INDEXABLE (class_oop))
	  {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	    /* return failure */
	    jit_movi (JIT_R0, -1);
	    return PRIM_FAIL | PRIM_INLINED;
	  }

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	jit_ldi (JIT_V1, &sp);
	jit_ldr (JIT_R1, JIT_V1);	/* load the argument */
	jit_movi (JIT_R0, -1);	/* failure */

	fail2 = jit_bmci (JIT_R1, 1);
	fail1 = jit_blti (JIT_R1, FROM_INT (0));

	jit_rshi (JIT_R1, JIT_R1, 1);	/* clear tag bit */
	jit_subi (JIT_V1, JIT_V1, sizeof (PTR));	/* set new
							   stack top */

	/* SET_STACKTOP (instantiate_oopwith (_gst_self, POP_OOP())) */
	jit_prepare ();
	jit_pushargr (JIT_V0);
	jit_pushargr (JIT_R1);
	jit_finishi (instantiate_oopwith);
	jit_retval (JIT_R0);

	/* Store the result and the new stack pointer */
	jit_str (JIT_V1, JIT_R0);
	jit_sti (&sp, JIT_V1);

	jit_movi (JIT_R0, 0);	/* success */

	jit_patch (fail2);
	jit_patch (fail1);

	return (PRIM_SUCCEED | PRIM_FAIL | PRIM_INLINED);
      }
#endif

    case 110:
      if (numArgs != 1)
	break;

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
      jit_ldi (JIT_V1, &sp);
      jit_ldr (JIT_R1, JIT_V1);	/* load the argument */
      jit_ner (JIT_R0, JIT_R1, JIT_V0);

      jit_subi (JIT_V1, JIT_V1, sizeof (PTR));	/* set new stack top */
      jit_lshi (JIT_V0, JIT_R0, LONG_SHIFT + 1);
      jit_movi (JIT_R0, 0);	/* success */
      jit_addi (JIT_V0, JIT_V0, (jit_word_t)_gst_true_oop);

      /* Store the result and the new stack pointer */
      jit_str (JIT_V1, JIT_V0);
      jit_sti (&sp, JIT_V1);

      return (PRIM_SUCCEED | PRIM_INLINED);

    case 111:
      {
	jit_node_t *jmp;
        if (numArgs != 0)
	  break;

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
        jit_ldi (JIT_V1, &sp);
	jit_movi (JIT_R0, (jit_word_t)_gst_small_integer_class);
        jmp = jit_bmsi (JIT_V0, 1);
        jit_ldxi (JIT_R0, JIT_V0, offsetof (struct oop_s, object));
        jit_ldxi (JIT_R0, JIT_R0, offsetof (struct object_s, objClass));
        jit_patch (jmp);

        /* Store the result and the new stack pointer */
        jit_movi (JIT_R0, 0);	/* success */
        jit_str (JIT_V1, JIT_V0);

        return (PRIM_SUCCEED | PRIM_INLINED);
      }
    }

  return (attr & ~PRIM_INLINED);
}

mst_Boolean
emit_primitive (int primitive, int numArgs)
{
  /* primitive */
  jit_node_t *fail, *succeed;
  prim_table_entry *pte = _gst_get_primitive_attributes (primitive);
  int attr = pte->attributes;

  if (attr & PRIM_INLINED)
    attr = emit_inlined_primitive (pte->id, numArgs, attr);

  if (!(attr & PRIM_INLINED))
    {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
      jit_movi (JIT_R1, numArgs);
      jit_movi (JIT_R2, pte->id);
      jit_prepare ();
      jit_pushargr (JIT_R2);
      jit_pushargr (JIT_R1);
      jit_finishi (pte->func);
      jit_retval (JIT_R0);
    }

  fail = ((attr & PRIM_FAIL)
	  && (attr & (PRIM_SUCCEED | PRIM_RELOAD_IP))) ?
    jit_beqi (JIT_R0, -1) : NULL;

  if (attr & PRIM_RELOAD_IP)
    {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
      succeed = (attr & PRIM_SUCCEED)
	? jit_beqi (JIT_R0, 0) : NULL;

      /* BlockClosure>>#value saves the native code's IP in the inline cache */
      if (attr & PRIM_CACHE_NEW_IP)
	jit_stxi (offsetof (inline_cache, cachedIP), JIT_V1, JIT_R0);

      jit_movr (JIT_V2, JIT_R0);

      if (succeed)
	jit_patch (succeed);
    }
  if (attr & (PRIM_SUCCEED | PRIM_RELOAD_IP))
    {
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
      if (attr & PRIM_CHECK_INTERRUPT)
	emit_interrupt_check (JIT_V2, -1);

      jit_jmpr (JIT_V2);
    }
  if (fail)
    jit_patch (fail);

  return !(attr & PRIM_FAIL);
}

void
emit_context_setup (int numArgs, int numTemps)
{
  if (numArgs > 3 || numTemps > 3)
    {
      /* Call through a loop written in C */
      jit_movi (JIT_V1, numTemps);
      jit_prepare ();
      jit_pushargr (JIT_R0);	/* newContext */
      jit_pushargr (JIT_V2);	/* numArgs */
      jit_pushargr (JIT_V1);	/* numTemps */
      jit_finishi (PTR_PREPARE_CONTEXT);
      IMPORT_SP;
      return;
    }

  /* Generate unrolled code to set up the frame -- this is done for
     about 95% of the methods.  */
  if (numArgs || numTemps)
    {
      int ofs;

      IMPORT_SP;
      switch (numArgs)
	{
	case 3:
	  jit_ldxi (JIT_V0, JIT_V2, -2 * sizeof (PTR));
	case 2:
	  jit_ldxi (JIT_R2, JIT_V2, -1 * sizeof (PTR));
	case 1:
	  jit_ldr (JIT_R1, JIT_V2);
	case 0:
	  break;
	}
      if (numTemps)
	jit_movi (JIT_V1, (jit_word_t)_gst_nil_oop);

      jit_addi (JIT_V2, JIT_R0,
		offsetof (struct gst_method_context, contextStack));
      jit_sti (&_gst_temporaries, JIT_V2);
      ofs = 0;
      switch (numArgs)
	{
	case 3:
	  jit_stxi (ofs, JIT_V2, JIT_V0);
	  ofs += sizeof (PTR);
	case 2:
	  jit_stxi (ofs, JIT_V2, JIT_R2);
	  ofs += sizeof (PTR);
	case 1:
	  jit_stxi (ofs, JIT_V2, JIT_R1);
	  ofs += sizeof (PTR);
	case 0:
	  break;
	}
      switch (numTemps)
	{
	case 3:
	  jit_stxi (ofs, JIT_V2, JIT_V1);
	  ofs += sizeof (PTR);
	case 2:
	  jit_stxi (ofs, JIT_V2, JIT_V1);
	  ofs += sizeof (PTR);
	case 1:
	  jit_stxi (ofs, JIT_V2, JIT_V1);
	  ofs += sizeof (PTR);
	case 0:
	  break;
	}

      jit_addi (JIT_V2, JIT_V2, ofs - sizeof (PTR));
    }
  else
    {
      jit_addi (JIT_V2, JIT_R0,
		offsetof (struct gst_method_context, contextStack) - sizeof (OOP));
    }
  jit_sti (&sp, JIT_V2);
}

void
emit_user_defined_method_call (OOP methodOOP, int numArgs,
			       gst_compiled_method method)
{
  int i;
  char *bp = method->bytecodes;
  static OOP arrayAssociation;

  current->inlineCaches = curr_inline_cache =
    (inline_cache *) xmalloc (2 * sizeof (inline_cache));

  /* Emit code similar to
     <method> valueWithReceiver: <self> withArguments: { arg1. arg2. ... } */

  if (!arrayAssociation)
    {
      arrayAssociation =
	dictionary_association_at (_gst_smalltalk_dictionary,
				   _gst_intern_string ("Array"));
    }

  t_sp = t_stack;
  push_tree_node_oop (bp, NULL, TREE_PUSH | TREE_LIT_CONST, methodOOP);
  push_tree_node (bp, NULL, TREE_PUSH | TREE_SELF, NULL);

  /* TODO: use instantiate_oop_with instead.  */
  push_tree_node_oop (bp, NULL, TREE_PUSH | TREE_LIT_VAR, arrayAssociation);
  push_tree_node_oop (bp, NULL, TREE_PUSH | TREE_LIT_CONST, FROM_INT (numArgs));
  push_send_node (bp, BYTECODE_SIZE, _gst_intern_string ("new:"), 1, false,
		  TREE_SEND | TREE_NORMAL, NEW_COLON_SPECIAL);

  for (i = 0; i < numArgs; i++)
    {
      push_tree_node (bp, NULL, TREE_PUSH | TREE_TEMP, (PTR) (uintptr_t) i);
      push_tree_node (bp, pop_tree_node (pop_tree_node (NULL)),
		      TREE_STORE | TREE_POP_INTO_ARRAY,
		      (PTR) (uintptr_t) i);
    }

  push_send_node (bp, BYTECODE_SIZE, _gst_value_with_rec_with_args_symbol, 2,
		  false, TREE_SEND | TREE_NORMAL, 0);

  set_top_node_extra (TREE_EXTRA_RETURN, 0);
  emit_code ();
  curr_inline_cache[-1].more = false;
}

mst_Boolean
emit_method_prolog (OOP methodOOP,
		    gst_compiled_method method)
{
  jit_node_t *jmp, *out;
  method_header header;
  int flag, stack_depth;
  OOP receiverClass;

  header = method->header;
  flag = header.headerFlag;
  receiverClass = current->receiverClass;

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
  if (flag == MTH_USER_DEFINED)
    /* Include enough stack slots for the arguments, the first
       two parameters of #valueWithReceiver:withArguments:,
       the Array class, and the parameter to #new:.  */
    stack_depth = ((header.numArgs + 4) + (1 << DEPTH_SCALE) - 1)
		  >> DEPTH_SCALE;
  else
    stack_depth = header.stack_depth;


  jit_ldxi (JIT_V0, JIT_V2, sizeof (PTR) * -header.numArgs);
  if (receiverClass == _gst_small_integer_class)
    out = jit_bmsi (JIT_V0, 1);
  else
    {
      jmp = jit_bmsi (JIT_V0, 1);
      jit_ldxi (JIT_R2, JIT_V0, offsetof (struct oop_s, object));
      jit_ldxi (JIT_R1, JIT_R2, offsetof (struct object_s, objClass));
      out = jit_beqi (JIT_R1, (jit_word_t)receiverClass);
      jit_patch (jmp);
    }
  jit_patch_abs (jit_jmpi (), do_send_code);
  jit_patch (out);

  /* Mark the translation as used *before* a GC can be triggered.  */
  jit_ldi (JIT_R0, &(methodOOP->flags));
  jit_ori (JIT_R0, JIT_R0, F_XLAT_REACHABLE);
  jit_sti (&(methodOOP->flags), JIT_R0);

  switch (flag)
    {
    case MTH_RETURN_SELF:
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
      jit_ldxi (JIT_V1, JIT_V1, offsetof (inline_cache, native_ip));
      jit_jmpr (JIT_V1);
      return (true);

    case MTH_RETURN_INSTVAR:
      {
	int ofs = offsetof (struct object_s, data)
	  + (header.primitiveIndex * sizeof (OOP));
	jit_ldxi (JIT_V1, JIT_V1, offsetof (inline_cache, native_ip));

#if DISASSEMBLE
  jit_note ("Remember3", __LINE__);
#endif
	jit_ldxi (JIT_R2, JIT_R2, ofs);	/* Remember? R2 is _gst_self->object */

	jit_str (JIT_V2, JIT_R2);	/* Make it the stack top */
	jit_jmpr (JIT_V1);
	return (true);
      }
      
    case MTH_RETURN_LITERAL:
      {
	OOP literal = OOP_TO_OBJ (method->literals)->data[header.primitiveIndex];
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
	jit_ldxi (JIT_V1, JIT_V1, offsetof (inline_cache, native_ip));
	jit_movi (JIT_R2, (jit_word_t)literal);
	jit_str (JIT_V2, JIT_R2);	/* Make it the stack top */
	jit_jmpr (JIT_V1);
	return (true);
      }
      
    default:
      break;
    }

#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
  jit_ldxi (JIT_V2, JIT_V1, offsetof (inline_cache, native_ip));

  if (flag == MTH_PRIMITIVE)
    if (emit_primitive (header.primitiveIndex, header.numArgs))
      return (true);

  /* Save the return IP */
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
  jit_ldi (JIT_R0, &_gst_this_context_oop);
  jit_ldxi (JIT_R0, JIT_R0, offsetof (struct oop_s, object));
  jit_addi (JIT_V2, JIT_V2, 1);
  jit_stxi (offsetof (struct gst_method_context, native_ip), JIT_R0,
	    JIT_V2);

  /* Prepare new state */
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
  jit_movi (JIT_R0, stack_depth);
  jit_movi (JIT_V2, header.numArgs);
  jit_prepare ();
  jit_pushargr (JIT_R0);
  jit_pushargr (JIT_V2);
  jit_finishi (PTR_ACTIVATE_NEW_CONTEXT);
  jit_retval (JIT_R0);

  /* Remember? V0 was loaded with _gst_self for the inline cache test */
#if DISASSEMBLE
  jit_note ("Remember0", __LINE__);
#endif
  jit_sti (&_gst_self, JIT_V0);

  /* Set the new context's flags, and _gst_this_method */
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
  jit_movi (JIT_V0, (jit_word_t)current->methodOOP);
  jit_movi (JIT_V1, (jit_word_t)MCF_IS_METHOD_CONTEXT);
  jit_sti (&_gst_this_method, JIT_V0);
  jit_stxi (offsetof (struct gst_method_context, flags), JIT_R0,
	    JIT_V1);

  /* Move the arguments and nil the temporaries */
#if DISASSEMBLE
  jit_note (method_name, __LINE__);
#endif
  emit_context_setup (header.numArgs, header.numTemps);

  define_ip_map_entry (0);
  emit_interrupt_check (JIT_NOREG, 0);

  /* For simplicity, we emit user-defined methods by creating a code_tree
     for the acrual send of #valueWithReceiver:withArguments: that they do.
     This requires creating the context, so we translate it now; otherwise
     it is very similar to a non-failing primitive.  */
  if (flag == MTH_USER_DEFINED)
    {
      emit_user_defined_method_call (methodOOP, header.numArgs, method);
      return (true);
    }

  return (false);
}

mst_Boolean
emit_block_prolog (OOP blockOOP,
		   gst_compiled_block block)
{
  block_header header;
  OOP receiverClass;
  jit_node_t *jmp, *out, *send;

  header = block->header;
  receiverClass = current->receiverClass;

  /* Check if the number of arguments matches ours */
  jit_ldxi_uc (JIT_R2, JIT_V1, offsetof (inline_cache, numArgs));
  out = jit_beqi (JIT_R2, header.numArgs);

  /* If they don't, check if we came here because somebody called
     send_block_value.  In this case, the number of arguments is surely 
     valid and the inline cache's numArgs is bogus. This handles
     #valueWithArguments:, #primCompile:ifError: and other primitives 
     in which send_block_value is used.  */
  jit_ldi (JIT_R2, &native_ip);
  jmp = jit_movi (JIT_R0, 0);
  jit_patch_at (jmp, native_code_label);
  jmp = jit_bner (JIT_R2, JIT_R0);
  send = jit_forward ();
  jit_patch_at (jmp, send);
  jit_patch (out);

  /* Check if a block evaluation was indeed requested, and if the
     BlockClosure really points to this CompiledBlock */
  jit_ldxi (JIT_R1, JIT_V2, sizeof (PTR) * -header.numArgs);
  jmp = jit_bmsi (JIT_R1, 1);
  jit_patch_at (jmp, send);
  jit_ldxi (JIT_R1, JIT_R1, offsetof (struct oop_s, object));
  jit_ldxi (JIT_R0, JIT_R1, offsetof (struct object_s, objClass));
  jit_ldxi (JIT_R2, JIT_R1, offsetof (struct gst_block_closure, block));
  jmp = jit_bnei (JIT_R0, (jit_word_t)_gst_block_closure_class);
  jit_patch_at (jmp, send);
  out = jit_beqi (JIT_R2, (jit_word_t)current->methodOOP);
  jit_link (send);
  jit_patch_abs (jit_jmpi (), do_send_code);
  jit_patch (out);

  /* Now, the standard class check.  Always load _gst_self, but don't
     check the receiver's class for clean blocks.  */
  jit_ldxi (JIT_V0, JIT_R1,
	    offsetof (struct gst_block_closure, receiver));
  if (block->header.clean != 0)
    {
      if (receiverClass == _gst_small_integer_class)
        {
	  out = jit_bmsi (JIT_V0, 1);
        }
      else
	{
	  jmp = jit_bmsi (JIT_V0, 1);
	  jit_ldxi (JIT_R0, JIT_V0, offsetof (struct oop_s, object));
	  jit_ldxi (JIT_R0, JIT_R0,
		    offsetof (struct object_s, objClass));
	  out = jit_beqi (JIT_R0, (jit_word_t)receiverClass);
	  jit_patch (jmp);
	}
      jit_patch_abs (jit_jmpi (), do_send_code);
      jit_patch (out);
    }

  /* Mark the translation as used *before* a GC can be triggered.  */
  jit_ldi (JIT_R0, &(blockOOP->flags));
  jit_ori (JIT_R0, JIT_R0, F_XLAT_REACHABLE);
  jit_sti (&(blockOOP->flags), JIT_R0);

  /* All tests passed.  Now save the return IP */
  jit_ldxi (JIT_V2, JIT_V1, offsetof (inline_cache, native_ip));
  jit_ldi (JIT_R0, &_gst_this_context_oop);
  jit_ldxi (JIT_R0, JIT_R0, offsetof (struct oop_s, object));
  jit_addi (JIT_V2, JIT_V2, 1);
  jit_stxi (offsetof (struct gst_method_context, native_ip), JIT_R0,
	    JIT_V2);

  /* Get the outer context in a callee-preserved register...  */
  jit_ldxi (JIT_V1, JIT_R1,
	    offsetof (struct gst_block_closure, outerContext));

  /* prepare new state */
  jit_movi (JIT_R0, header.depth);
  jit_movi (JIT_V2, header.numArgs);
  jit_prepare ();
  jit_pushargr (JIT_R0);
  jit_pushargr (JIT_V2);
  jit_finishi (PTR_ACTIVATE_NEW_CONTEXT);
  jit_retval (JIT_R0);

  /* Remember? V0 was loaded with _gst_self for the inline cache test.
     Also initialize _gst_this_method and the pointer to the
     outerContext.  */
  jit_movi (JIT_R1, (jit_word_t)current->methodOOP);
#if DISASSEMBLE
  jit_note("Remember1", __LINE__);
#endif
  jit_sti (&_gst_self, JIT_V0);
  jit_sti (&_gst_this_method, JIT_R1);
  jit_stxi (offsetof (struct gst_block_context, outerContext), JIT_R0,
	    JIT_V1);

  /* Move the arguments and nil the temporaries */
  emit_context_setup (header.numArgs, header.numTemps);

  define_ip_map_entry (0);
  emit_interrupt_check (JIT_NOREG, 0);

  return (false);
}


/* Code tree creation */

gst_uchar *
decode_bytecode (gst_uchar *bp)
{
  static OOP *specialOOPs[] = {
    &_gst_nil_oop, &_gst_true_oop, &_gst_false_oop
  };

  MATCH_BYTECODES (XLAT_BUILD_CODE_TREE, bp, (
    PUSH_RECEIVER_VARIABLE {
      push_tree_node (IP0, NULL, TREE_PUSH | TREE_REC_VAR,
                      (PTR) (uintptr_t) n);
    }

    PUSH_TEMPORARY_VARIABLE {
      push_tree_node (IP0, NULL, TREE_PUSH | TREE_TEMP,
                      (PTR) (uintptr_t) n);
    }

    PUSH_LIT_CONSTANT {
      push_tree_node_oop (IP0, NULL, TREE_PUSH | TREE_LIT_CONST,
                          literals[n]);
    }

    PUSH_LIT_VARIABLE {
      if (is_a_kind_of (OOP_INT_CLASS (literals[n]), _gst_association_class))
        push_tree_node_oop (IP0, NULL, TREE_PUSH | TREE_LIT_VAR,
                            literals[n]);
      else
	{
          push_tree_node_oop (IP0, NULL, TREE_PUSH | TREE_LIT_CONST,
                              literals[n]);
          push_send_node (IP0, IP - IP0, _gst_builtin_selectors[VALUE_SPECIAL].symbol,
			  0, false, TREE_SEND, 0);
	}
    }

    PUSH_SELF {
      push_tree_node (IP0, NULL,
		      TREE_PUSH | TREE_SELF | self_class_check, NULL);
    }
    PUSH_SPECIAL {
      push_tree_node_oop (IP0, NULL, TREE_PUSH | TREE_LIT_CONST,
                          *specialOOPs[n]);
    }
    PUSH_INTEGER {
      push_tree_node_oop (IP0, NULL, TREE_PUSH | TREE_LIT_CONST,
                          FROM_INT (n));
    }

    RETURN_METHOD_STACK_TOP {
      set_top_node_extra (TREE_EXTRA_METHOD_RET, 0);
      emit_code ();
    }
    RETURN_CONTEXT_STACK_TOP {
      set_top_node_extra (TREE_EXTRA_RETURN, 0);
      emit_code ();
    }

    LINE_NUMBER_BYTECODE {
    }

    STORE_RECEIVER_VARIABLE {
      push_tree_node (IP0, pop_tree_node (NULL),
                      TREE_STORE | TREE_REC_VAR,
                      (PTR) (uintptr_t) n);
    }
    STORE_TEMPORARY_VARIABLE {
      push_tree_node (IP0, pop_tree_node (NULL),
                      TREE_STORE | TREE_TEMP,
                      (PTR) (uintptr_t) n);
    }
    STORE_LIT_VARIABLE {
      if (is_a_kind_of (OOP_INT_CLASS (literals[n]), _gst_association_class))
        push_tree_node_oop (IP0, pop_tree_node (NULL),
			    TREE_STORE | TREE_LIT_VAR, literals[n]);
      else
	{
	  code_tree *value, *var;
	  inline_cache *ic;

          push_tree_node_oop (IP0, NULL,
			      TREE_ALT_PUSH | TREE_LIT_CONST, literals[n]);
          ic = set_inline_cache (_gst_builtin_selectors[VALUE_COLON_SPECIAL].symbol,
			         1, false, TREE_SEND, 0);

	  var = pop_tree_node (NULL);
	  value = pop_tree_node (var);
	  push_tree_node (IP0, value, TREE_SEND | TREE_STORE_LIT_VAR, (PTR) ic);
	}
    }

    SEND {
      push_send_node (IP0, IP - IP0, literals[n], num_args, super, TREE_SEND, 0);
    }

    POP_INTO_NEW_STACKTOP {
      push_tree_node (IP0,
                      pop_tree_node (pop_tree_node (NULL)),
                      TREE_STORE | TREE_POP_INTO_ARRAY,
                      (PTR) (uintptr_t) n);
    }

    POP_STACK_TOP {
      set_top_node_extra (TREE_EXTRA_POP, 0);
      emit_code ();

      /* This is very important!  If we do not adjust T_SP here, we
         miscompile superoperators that include a POP/PUSH sequence.  */
      t_sp--;
    }
    DUP_STACK_TOP {
      push_tree_node (IP0, NULL, TREE_PUSH | TREE_DUP, NULL);
    }

    PUSH_OUTER_TEMP {
      push_tree_node (IP0, NULL, TREE_PUSH | TREE_OUTER_TEMP,
		      (PTR) (uintptr_t) ((scopes << 8) | n));
    }
    STORE_OUTER_TEMP {
      push_tree_node (IP0,
                      pop_tree_node (NULL),
                      TREE_STORE | TREE_OUTER_TEMP, 
		      (PTR) (uintptr_t) ((scopes << 8) | n));
    }

    JUMP {
      set_top_node_extra (TREE_EXTRA_JMP_ALWAYS, ofs);

      emit_code ();
    }
    POP_JUMP_TRUE {
      set_top_node_extra (TREE_EXTRA_JMP_TRUE, ofs);

      emit_code ();
    }
    POP_JUMP_FALSE {
      set_top_node_extra (TREE_EXTRA_JMP_FALSE, ofs);

      emit_code ();
    }

    SEND_ARITH {
      int op = special_send_bytecodes[n];
      const struct builtin_selector *bs = &_gst_builtin_selectors[n];
      push_send_node (IP0, IP - IP0, bs->symbol, bs->numArgs, false, op, n);
    }
    SEND_SPECIAL {
      int op = special_send_bytecodes[n + 16];
      const struct builtin_selector *bs = &_gst_builtin_selectors[n + 16];
      push_send_node (IP0, IP - IP0, bs->symbol, bs->numArgs, false, op, n + 16);
    }
    SEND_IMMEDIATE {
      const struct builtin_selector *bs = &_gst_builtin_selectors[n];
      push_send_node (IP0, IP - IP0, bs->symbol, bs->numArgs, super,
                      TREE_SEND | TREE_NORMAL, n);
    }

    MAKE_DIRTY_BLOCK {
      code_tree *arg;
      arg = pop_tree_node (NULL);
      push_tree_node (IP0, arg, TREE_SEND | TREE_DIRTY_BLOCK, NULL);
    }

    EXIT_INTERPRETER, INVALID {
      abort ();
    }
  ));

#if 0
    /* These used to be here but we do not produce them anymore.  It would
       speed up the code a bit, so they are kept here as a remainder.  */

    REPLACE_SELF {
      push_tree_node (IP0,
                      pop_tree_node (NULL),
                      TREE_SET_TOP | TREE_SELF | self_class_check, NULL);

      emit_code ();
    }
    REPLACE_ONE {
      push_tree_node_oop (IP0,
                          pop_tree_node (NULL),
                          TREE_SET_TOP | TREE_LIT_CONST, FROM_INT (1));

      emit_code ();
    }

    REPLACE_RECEIVER_VARIABLE {
      push_tree_node (IP0,
                      pop_tree_node (NULL),
                      TREE_SET_TOP | TREE_REC_VAR,
                      (PTR) (uintptr_t) n);

      emit_code ();
    }
    REPLACE_TEMPORARY_VARIABLE {
      push_tree_node (IP0,
                      pop_tree_node (NULL),
                      TREE_SET_TOP | TREE_TEMP,
                      (PTR) (uintptr_t) n);

      emit_code ();
    }
    REPLACE_LIT_CONSTANT {
      push_tree_node (IP0,
                      pop_tree_node (NULL),
                      TREE_SET_TOP | TREE_LIT_CONST,
                      literals[n]);

      emit_code ();
    }
    REPLACE_LIT_VARIABLE {
      push_tree_node (IP0,
                      pop_tree_node (NULL),
                      TREE_SET_TOP | TREE_LIT_VAR,
                      literals[n]);

      emit_code ();
    }
#endif

  return bp;
}




/* Main translator loop */
void
translate_method (OOP methodOOP, OOP receiverClass, int size)
{
  gst_uchar *end, *bp, *bp_first;
  int inlineCacheCount;
  char *destinations;
  code_stack_pointer *stackPos;
  int i;

  rec_var_cached = self_cached = false;
  stack_cached = -1;
  sp_delta = -(int)sizeof (PTR);
  deferred_head = NULL;
  method_class = GET_METHOD_CLASS (methodOOP);
  bc = GET_METHOD_BYTECODES (methodOOP);
  literals = GET_METHOD_LITERALS (methodOOP);
  end = bc + size;

  if (receiverClass == _gst_small_integer_class)
    self_class_check = TREE_IS_INTEGER;

  else
    self_class_check = TREE_IS_NOT_INTEGER;

  /* Emit the prolog of the compiled code.  */
  jit_ldi (JIT_V2, &sp);
  if (OOP_CLASS (methodOOP) == _gst_compiled_block_class)
    {
      if (emit_block_prolog
	  (methodOOP, (gst_compiled_block) OOP_TO_OBJ (methodOOP)))
	return;
    }
  else
    {
      if (emit_method_prolog
	  (methodOOP, (gst_compiled_method) OOP_TO_OBJ (methodOOP)))
	return;
    }


  /* Count the space for the inline caches */
  for (inlineCacheCount = 0, bp = bc; bp < end; )
    MATCH_BYTECODES (XLAT_COUNT_SENDS, bp, (
      PUSH_RECEIVER_VARIABLE, PUSH_TEMPORARY_VARIABLE,
      PUSH_LIT_CONSTANT, PUSH_SELF,
      PUSH_SPECIAL, PUSH_INTEGER, RETURN_METHOD_STACK_TOP,
      RETURN_CONTEXT_STACK_TOP, LINE_NUMBER_BYTECODE,
      STORE_RECEIVER_VARIABLE, STORE_TEMPORARY_VARIABLE,
      POP_INTO_NEW_STACKTOP,
      POP_STACK_TOP, DUP_STACK_TOP, PUSH_OUTER_TEMP,
      STORE_OUTER_TEMP, JUMP, POP_JUMP_TRUE, POP_JUMP_FALSE,
      MAKE_DIRTY_BLOCK, EXIT_INTERPRETER, INVALID { }

      PUSH_LIT_VARIABLE, STORE_LIT_VARIABLE {
	if (!is_a_kind_of (OOP_INT_CLASS (literals[n]), _gst_association_class))
	  inlineCacheCount++;
      }

      SEND_ARITH, SEND_SPECIAL, SEND_IMMEDIATE, SEND {
        inlineCacheCount++;
      }
    ));


  if (inlineCacheCount)
    {
      current->inlineCaches = curr_inline_cache =
	(inline_cache *) xmalloc (inlineCacheCount *
				  sizeof (inline_cache));
    }

  stackPos = alloca ((1 + size) * sizeof (code_stack_pointer *));
  labels = alloca ((1 + size) * sizeof (jit_node_t *));
  destinations = (char *) labels;

  _gst_compute_stack_positions (bc, size, (PTR *) t_stack, (PTR **) stackPos);
  _gst_analyze_bytecodes (methodOOP, size, destinations);

  /* Create labels for bytecodes on which a jump lands */
  for (i = size; --i >= 0;)
    labels[i] = destinations[i] ? jit_forward () : NULL;

  /* Now, go through the main translation loop */
  for (bp = bc, this_label = labels; bp < end; )
    {
      if (!*stackPos)
	{
	  assert (!*this_label);
	  this_label += 2;
	  stackPos += 2;
	  continue;
	}

      /* Updating the t_sp in push_tree_node/pop_tree_node is not
         enough, because if two basic blocks are mutually exclusive the
         SP at the second block's entrance must be the same as the SP at 
         the first block's entrance, even if the blocks have a non-zero
         stack balance.  */
      t_sp = *stackPos;

      if (*this_label)
	{
	  /* A basic block ends here. Compile it.  */
	  emit_code ();
	  CACHE_STACK_TOP;

	  /* If the label was not used yet, it will be used for a
	     backward jump.  A backward jump could be used to code an
	     infinite loop such as `[] repeat', so we test
	     _gst_except_flag here.  */
	  jit_link (*this_label);
	  if (!jit_target_p (*this_label))
	    {
	      define_ip_map_entry (bp - bc);
	      emit_interrupt_check (JIT_NOREG, bp - bc);
	    }
	}

      bp_first = bp;
      bp = decode_bytecode (bp);
      this_label += bp - bp_first;
      stackPos += bp - bp_first;
    }

  emit_code ();
  emit_deferred_sends (deferred_head);

  if (inlineCacheCount)
    curr_inline_cache[-1].more = false;
}


/* External interfacing */

void
_gst_init_translator (void)
{
  static mst_Boolean initialized = false;

  if (!initialized)
    {
      initialized = true;
      generate_run_time_code ();
      memset (methods_table, 0, sizeof (methods_table));
    }
}

PTR
_gst_map_virtual_ip (OOP methodOOP, OOP receiverClass, int ip)
{
  ip_map *map;
  method_entry *method;

  method = find_method_entry (methodOOP, receiverClass);

  map = method->ipMap;
  if (!map)
    return NULL;

  do
    if (map->virtualIP == ip)
      return map->native_ip;
  while ((++map)->native_ip);

  return NULL;
}

PTR
_gst_get_native_code (OOP methodOOP, OOP receiverClass)
{
  if (!IS_OOP_VERIFIED (methodOOP))
    _gst_verify_sent_method (methodOOP);

  return find_method_entry (methodOOP, receiverClass)->nativeCode;
}

method_entry *
find_method_entry (OOP methodOOP, OOP receiverClass)
{
  method_entry *method, *prev;
  unsigned int hashEntry;
  int size;

  if (IS_NIL (methodOOP))
    return (NULL);

  hashEntry = OOP_INDEX (methodOOP) % HASH_TABLE_SIZE;
  if ((method = methods_table[hashEntry]))
    {
      if (method->methodOOP == methodOOP
	  && method->receiverClass == receiverClass)
	return method;

      for (prev = method; (method = method->next); prev = method)
	{
	  if (method->methodOOP != methodOOP
	      || method->receiverClass != receiverClass)
	    continue;

	  prev->next = method->next;
	  method->next = methods_table[hashEntry];
	  methods_table[hashEntry] = method;
	  return method;
	}
    }

  size = NUM_INDEXABLE_FIELDS (methodOOP);
  new_method_entry (methodOOP, receiverClass);
  translate_method (methodOOP, receiverClass, size);
  return (finish_method_entry ());
}

void
reset_invalidated_inline_caches ()
{
  method_entry *method, **hashEntry;
  inline_cache *ic;
  void *lookupIP;

  for (hashEntry = methods_table; hashEntry <= &discarded; hashEntry++)
    for (method = *hashEntry; method; method = method->next)
      {
        ic = method->inlineCaches;
        if (!ic)
	  continue;

        do
	  {
	    lookupIP = ic->is_super ? do_super_code : do_send_code;
	    if (ic->cachedIP != lookupIP && !get_native_entry (ic->cachedIP))
	      ic->cachedIP = lookupIP;
	  }
        while ((ic++)->more);
      }
}

void
_gst_reset_inline_caches ()
{
  method_entry *method, **hashEntry;
  inline_cache *ic;

  for (hashEntry = methods_table; hashEntry <= &discarded; hashEntry++)
    for (method = *hashEntry; method; method = method->next)
      {
        ic = method->inlineCaches;
        if (!ic)
	  continue;

        do
	  ic->cachedIP = ic->is_super ? do_super_code : do_send_code;
        while ((ic++)->more);
      }
}

void
_gst_free_released_native_code (void)
{
  method_entry *method;

  if (!released)
    return;

  reset_invalidated_inline_caches ();
  _gst_validate_method_cache_entries ();

  /* now free the list */
  while ((method = released))
    {
      released = released->next;
#define _jit method->_jit
      jit_destroy_state ();
#undef _jit
      method->_jit = NULL;
      xfree (method);
    }
}

void
walk_and_remove_method (OOP methodOOP, method_entry **ptrNext)
{
  method_entry *method;

  while ((method = *ptrNext))
    {
      if (method->methodOOP != methodOOP)
	{
	  /* Move ptrNext forward */
	  ptrNext = &(method->next);
	  continue;
	}

      /* Adjust the list */
      *ptrNext = method->next;
      method->next = released;
      released = method;

      /* Mark the method as freed */
      if (method->inlineCaches)
	xfree (method->inlineCaches);

      rem_native_entry (method);
      method->receiverClass = NULL;
      method->inlineCaches = NULL;
    }

  /* Terminate the list */
  *ptrNext = NULL;
}

void
_gst_release_native_code (OOP methodOOP)
{
  unsigned int hashEntry;

  hashEntry = OOP_INDEX (methodOOP) % HASH_TABLE_SIZE;
  walk_and_remove_method (methodOOP, &methods_table[hashEntry]);
  methodOOP->flags &= ~F_XLAT;

  if (methodOOP->flags & F_XLAT_DISCARDED)
    {
      walk_and_remove_method (methodOOP, &discarded);
      methodOOP->flags &= ~F_XLAT_DISCARDED;
    }
}

void
_gst_discard_native_code (OOP methodOOP)
{
  method_entry *method, **ptrNext;
  unsigned int hashEntry;

  methodOOP->flags |= F_XLAT_DISCARDED;
  hashEntry = OOP_INDEX (methodOOP) % HASH_TABLE_SIZE;
  ptrNext = &methods_table[hashEntry];

  while ((method = *ptrNext))
    {
      if (method->methodOOP != methodOOP)
	{
	  /* Move ptrNext forward */
	  ptrNext = &(method->next);
	  continue;
	}

      assert (methodOOP->flags & F_XLAT);

      /* Move to the `discarded' list */
      *ptrNext = method->next;
      method->next = discarded;
      discarded = method;
    }

  /* Terminate the list */
  *ptrNext = NULL;
}

#endif /* ENABLE_JIT_TRANSLATION */
