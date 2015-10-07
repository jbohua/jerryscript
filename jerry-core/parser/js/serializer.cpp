/* Copyright 2014-2015 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lit-id-hash-table.h"
#include "serializer.h"
#include "bytecode-data.h"
#include "pretty-printer.h"
#include "array-list.h"
#include "scopes-tree.h"

static bytecode_data_header_t *first_bytecode_header_p;
static scopes_tree current_scope;
static bool print_instrs;

static void
serializer_print_instrs (const bytecode_data_header_t *);

op_meta
serializer_get_op_meta (vm_instr_counter_t oc)
{
  JERRY_ASSERT (current_scope);
  return scopes_tree_op_meta (current_scope, oc);
}

/**
 * Get variable declaration of the current scope
 *
 * @return variable declaration instruction
 */
op_meta
serializer_get_var_decl (vm_instr_counter_t oc) /**< index of variable declaration */
{
  JERRY_ASSERT (current_scope);
  return scopes_tree_var_decl (current_scope, oc);
} /* serializer_get_var_decl */

/**
 * Get byte-code instruction from current scope, or specified byte-code array
 *
 * @return byte-code instruction
 */
vm_instr_t
serializer_get_instr (const bytecode_data_header_t *bytecode_data_p, /**< pointer to byte-code data (or NULL,
                                                                      *   if instruction should be taken from
                                                                      *   instruction list of current scope) */
                      vm_instr_counter_t oc) /**< position of the intruction */
{
  if (bytecode_data_p == NULL)
  {
    return serializer_get_op_meta (oc).op;
  }
  else
  {
    JERRY_ASSERT (oc < bytecode_data_p->instrs_count);
    return bytecode_data_p->instrs_p[oc];
  }
} /* serializer_get_instr */

/**
 * Convert literal id (operand value of instruction) to compressed pointer to literal
 *
 * Bytecode is divided into blocks of fixed size and each block has independent encoding of variable names,
 * which are represented by 8 bit numbers - ids.
 * This function performs conversion from id to literal.
 *
 * @return compressed pointer to literal
 */
lit_cpointer_t
serializer_get_literal_cp_by_uid (uint8_t id, /**< literal idx */
                                  const bytecode_data_header_t *bytecode_data_p, /**< pointer to bytecode */
                                  vm_instr_counter_t oc) /**< position in the bytecode */
{
  lit_id_hash_table *lit_id_hash = null_hash;
  if (bytecode_data_p)
  {
    lit_id_hash = MEM_CP_GET_POINTER (lit_id_hash_table, bytecode_data_p->lit_id_hash_cp);
  }
  else
  {
    lit_id_hash = MEM_CP_GET_POINTER (lit_id_hash_table, first_bytecode_header_p->lit_id_hash_cp);
  }

  if (lit_id_hash == null_hash)
  {
    return INVALID_LITERAL;
  }

  return lit_id_hash_table_lookup (lit_id_hash, id, oc);
} /* serializer_get_literal_cp_by_uid */

void
serializer_set_scope (scopes_tree new_scope)
{
  current_scope = new_scope;
}

/**
 * Dump scope to current scope
 *
 * NOTE:
 *   This function is used for processing of function expressions as they should not be hoisted.
 *   After parsing a function expression, it is immediately dumped to current scope via call of this function.
 */
void
serializer_dump_subscope (scopes_tree tree) /**< scope to dump */
{
  JERRY_ASSERT (tree != NULL);
  vm_instr_counter_t instr_pos;
  bool header = true;
  for (instr_pos = 0; instr_pos < tree->instrs_count; instr_pos++)
  {
    op_meta *om_p = (op_meta *) linked_list_element (tree->instrs, instr_pos);
    if (om_p->op.op_idx != VM_OP_VAR_DECL
        && om_p->op.op_idx != VM_OP_META && !header)
    {
      break;
    }
    if (om_p->op.op_idx == VM_OP_REG_VAR_DECL)
    {
      header = false;
    }
    scopes_tree_add_op_meta (current_scope, *om_p);
  }
  for (vm_instr_counter_t var_decl_pos = 0;
       var_decl_pos < linked_list_get_length (tree->var_decls);
       var_decl_pos++)
  {
    op_meta *om_p = (op_meta *) linked_list_element (tree->var_decls, var_decl_pos);
    scopes_tree_add_op_meta (current_scope, *om_p);
  }
  for (uint8_t child_id = 0; child_id < tree->t.children_num; child_id++)
  {
    serializer_dump_subscope (*(scopes_tree *) linked_list_element (tree->t.children, child_id));
  }
  for (; instr_pos < tree->instrs_count; instr_pos++)
  {
    op_meta *om_p = (op_meta *) linked_list_element (tree->instrs, instr_pos);
    scopes_tree_add_op_meta (current_scope, *om_p);
  }
} /* serializer_dump_subscope */


/**
 * Merge scopes tree into bytecode
 *
 * @return pointer to generated bytecode
 */
const bytecode_data_header_t *
serializer_merge_scopes_into_bytecode (void)
{
  const size_t buckets_count = scopes_tree_count_literals_in_blocks (current_scope);
  const vm_instr_counter_t instrs_count = scopes_tree_count_instructions (current_scope);
  const size_t blocks_count = JERRY_ALIGNUP (instrs_count, BLOCK_SIZE) / BLOCK_SIZE;

  const size_t bytecode_size = JERRY_ALIGNUP (instrs_count * sizeof (vm_instr_t), MEM_ALIGNMENT);
  const size_t hash_table_size = lit_id_hash_table_get_size_for_table (buckets_count, blocks_count);
  const size_t header_and_hash_table_size = JERRY_ALIGNUP (sizeof (bytecode_data_header_t) + hash_table_size,
                                                           MEM_ALIGNMENT);

  uint8_t *buffer_p = (uint8_t*) mem_heap_alloc_block (bytecode_size + header_and_hash_table_size,
                                                       MEM_HEAP_ALLOC_LONG_TERM);

  lit_id_hash_table *lit_id_hash = lit_id_hash_table_init (buffer_p + sizeof (bytecode_data_header_t),
                                                           hash_table_size,
                                                           buckets_count, blocks_count);

  vm_instr_t *bytecode_p = scopes_tree_raw_data (current_scope,
                                                 buffer_p + header_and_hash_table_size,
                                                 bytecode_size,
                                                 lit_id_hash);

  bytecode_data_header_t *header_p = (bytecode_data_header_t *) buffer_p;
  MEM_CP_SET_POINTER (header_p->lit_id_hash_cp, lit_id_hash);
  header_p->instrs_p = bytecode_p;
  header_p->instrs_count = instrs_count;
  MEM_CP_SET_POINTER (header_p->next_header_cp, first_bytecode_header_p);

  first_bytecode_header_p = header_p;

  if (print_instrs)
  {
    lit_dump_literals ();
    serializer_print_instrs (header_p);
  }

  return header_p;
} /* serializer_merge_scopes_into_bytecode */

void
serializer_dump_op_meta (op_meta op)
{
  JERRY_ASSERT (scopes_tree_instrs_num (current_scope) < MAX_OPCODES);

  scopes_tree_add_op_meta (current_scope, op);

#ifdef JERRY_ENABLE_PRETTY_PRINTER
  if (print_instrs)
  {
    pp_op_meta (NULL, (vm_instr_counter_t) (scopes_tree_instrs_num (current_scope) - 1), op, false);
  }
#endif
}

/**
 * Dump variable declaration into the current scope
 */
void
serializer_dump_var_decl (op_meta op) /**< variable declaration instruction */
{
  JERRY_ASSERT (scopes_tree_instrs_num (current_scope)
                + linked_list_get_length (current_scope->var_decls) < MAX_OPCODES);

  scopes_tree_add_var_decl (current_scope, op);
} /* serializer_dump_var_decl */

vm_instr_counter_t
serializer_get_current_instr_counter (void)
{
  return scopes_tree_instrs_num (current_scope);
}

/**
 * Get number of variable declarations in the current scope
 *
 * @return count of variable declarations
 */
vm_instr_counter_t
serializer_get_current_var_decls_counter (void)
{
  return scopes_tree_var_decls_num (current_scope);
} /* serializer_get_current_var_decls_counter */

vm_instr_counter_t
serializer_count_instrs_in_subscopes (void)
{
  return (vm_instr_counter_t) (scopes_tree_count_instructions (current_scope) - scopes_tree_instrs_num (current_scope));
}

void
serializer_set_writing_position (vm_instr_counter_t oc)
{
  scopes_tree_set_instrs_num (current_scope, oc);
}

void
serializer_rewrite_op_meta (const vm_instr_counter_t loc, op_meta op)
{
  scopes_tree_set_op_meta (current_scope, loc, op);

#ifdef JERRY_ENABLE_PRETTY_PRINTER
  if (print_instrs)
  {
    pp_op_meta (NULL, loc, op, true);
  }
#endif
}

static void
serializer_print_instrs (const bytecode_data_header_t *bytecode_data_p)
{
#ifdef JERRY_ENABLE_PRETTY_PRINTER
  for (vm_instr_counter_t loc = 0; loc < bytecode_data_p->instrs_count; loc++)
  {
    op_meta opm;

    opm.op = bytecode_data_p->instrs_p[loc];
    for (int i = 0; i < 3; i++)
    {
      opm.lit_id[i] = NOT_A_LITERAL;
    }

    pp_op_meta (bytecode_data_p, loc, opm, false);
  }
#else
  (void) bytecode_data_p;
#endif
}

void
serializer_init ()
{
  current_scope = NULL;
  print_instrs = false;

  first_bytecode_header_p = NULL;

  lit_init ();
}

void serializer_set_show_instrs (bool show_instrs)
{
  print_instrs = show_instrs;
}

/**
 * Deletes bytecode and associated hash table
 */
void
serializer_remove_bytecode_data (const bytecode_data_header_t *bytecode_data_p) /**< pointer to bytecode data which
                                                                                 * should be deleted */
{
  bytecode_data_header_t *prev_header = NULL;
  bytecode_data_header_t *cur_header_p = first_bytecode_header_p;

  while (cur_header_p != NULL)
  {
    if (cur_header_p == bytecode_data_p)
    {
      if (prev_header)
      {
        prev_header->next_header_cp = cur_header_p->next_header_cp;
      }
      else
      {
        first_bytecode_header_p = MEM_CP_GET_POINTER (bytecode_data_header_t, cur_header_p->next_header_cp);
      }
      mem_heap_free_block (cur_header_p);
      break;
    }

    prev_header = cur_header_p;
    cur_header_p = MEM_CP_GET_POINTER (bytecode_data_header_t, cur_header_p->next_header_cp);
  }
} /* serializer_remove_instructions */

void
serializer_free (void)
{
  lit_finalize ();

  while (first_bytecode_header_p != NULL)
  {
    bytecode_data_header_t *header_p = first_bytecode_header_p;
    first_bytecode_header_p = MEM_CP_GET_POINTER (bytecode_data_header_t, header_p->next_header_cp);

    mem_heap_free_block (header_p);
  }
}

#ifdef JERRY_ENABLE_SNAPSHOT
/**
 * Dump byte-code and idx-to-literal map to snapshot
 *
 * @return true, upon success (i.e. buffer size is enough),
 *         false - otherwise.
 */
bool
serializer_dump_bytecode_with_idx_map (uint8_t *buffer_p, /**< buffer to dump to */
                                       size_t buffer_size, /**< buffer size */
                                       size_t *in_out_buffer_offset_p, /**< in-out: buffer write offset */
                                       const bytecode_data_header_t *bytecode_data_p, /**< byte-code data */
                                       const lit_mem_to_snapshot_id_map_entry_t *lit_map_p, /**< map from literal
                                                                                             *   identifiers in
                                                                                             *   literal storage
                                                                                             *   to literal offsets
                                                                                             *   in snapshot */
                                       uint32_t literals_num, /**< literals number */
                                       uint32_t *out_bytecode_size_p, /**< out: size of dumped instructions array */
                                       uint32_t *out_idx_to_lit_map_size_p) /**< out: side of dumped
                                                                             *        idx to literals map */
{
  JERRY_ASSERT (bytecode_data_p->next_header_cp == MEM_CP_NULL);

  vm_instr_counter_t instrs_num = bytecode_data_p->instrs_count;

  const size_t instrs_array_size = sizeof (vm_instr_t) * instrs_num;
  if (*in_out_buffer_offset_p + instrs_array_size > buffer_size)
  {
    return false;
  }
  memcpy (buffer_p + *in_out_buffer_offset_p, bytecode_data_p->instrs_p, instrs_array_size);
  *in_out_buffer_offset_p += instrs_array_size;

  *out_bytecode_size_p = (uint32_t) (sizeof (vm_instr_t) * instrs_num);

  lit_id_hash_table *lit_id_hash_p = MEM_CP_GET_POINTER (lit_id_hash_table, bytecode_data_p->lit_id_hash_cp);
  uint32_t idx_to_lit_map_size = lit_id_hash_table_dump_for_snapshot (buffer_p,
                                                                      buffer_size,
                                                                      in_out_buffer_offset_p,
                                                                      lit_id_hash_p,
                                                                      lit_map_p,
                                                                      literals_num,
                                                                      instrs_num);

  if (idx_to_lit_map_size == 0)
  {
    return false;
  }
  else
  {
    *out_idx_to_lit_map_size_p = idx_to_lit_map_size;

    return true;
  }
} /* serializer_dump_bytecode_with_idx_map */

/**
 * Register bytecode and idx map from snapshot
 *
 * NOTE:
 *      If is_copy flag is set, bytecode is copied from snapshot, else bytecode is referenced directly
 *      from snapshot
 *
 * @return pointer to byte-code header, upon success,
 *         NULL - upon failure (i.e., in case snapshot format is not valid)
 */
const bytecode_data_header_t *
serializer_load_bytecode_with_idx_map (const uint8_t *bytecode_and_idx_map_p, /**< buffer with instructions array
                                                                               *   and idx to literals map from
                                                                               *   snapshot */
                                       uint32_t bytecode_size, /**< size of instructions array */
                                       uint32_t idx_to_lit_map_size, /**< size of the idx to literals map */
                                       const lit_mem_to_snapshot_id_map_entry_t *lit_map_p, /**< map of in-snapshot
                                                                                             *   literal offsets
                                                                                             *   to literal identifiers,
                                                                                             *   created in literal
                                                                                             *   storage */
                                       uint32_t literals_num, /**< number of literals */
                                       bool is_copy) /** flag, indicating whether the passed in-snapshot data
                                                      *  should be copied to engine's memory (true),
                                                      *  or it can be referenced until engine is stopped
                                                      *  (i.e. until call to jerry_cleanup) */
{
  const uint8_t *idx_to_lit_map_p = bytecode_and_idx_map_p + bytecode_size;

  size_t instructions_number = bytecode_size / sizeof (vm_instr_t);
  size_t blocks_count = JERRY_ALIGNUP (instructions_number, BLOCK_SIZE) / BLOCK_SIZE;

  uint32_t idx_num_total;
  size_t idx_to_lit_map_offset = 0;
  if (!jrt_read_from_buffer_by_offset (idx_to_lit_map_p,
                                       idx_to_lit_map_size,
                                       &idx_to_lit_map_offset,
                                       &idx_num_total))
  {
    return NULL;
  }

  const size_t bytecode_alloc_size = JERRY_ALIGNUP (bytecode_size, MEM_ALIGNMENT);
  const size_t hash_table_size = lit_id_hash_table_get_size_for_table (idx_num_total, blocks_count);
  const size_t header_and_hash_table_size = JERRY_ALIGNUP (sizeof (bytecode_data_header_t) + hash_table_size,
                                                           MEM_ALIGNMENT);
  const size_t alloc_size = header_and_hash_table_size + (is_copy ? bytecode_alloc_size : 0);

  uint8_t *buffer_p = (uint8_t*) mem_heap_alloc_block (alloc_size, MEM_HEAP_ALLOC_LONG_TERM);
  bytecode_data_header_t *header_p = (bytecode_data_header_t *) buffer_p;

  vm_instr_t *instrs_p;
  vm_instr_t *snapshot_instrs_p = (vm_instr_t *) bytecode_and_idx_map_p;
  if (is_copy)
  {
    instrs_p = (vm_instr_t *) (buffer_p + header_and_hash_table_size);
    memcpy (instrs_p, snapshot_instrs_p, bytecode_size);
  }
  else
  {
    instrs_p = snapshot_instrs_p;
  }

  uint8_t *lit_id_hash_table_buffer_p = buffer_p + sizeof (bytecode_data_header_t);
  if (lit_id_hash_table_load_from_snapshot (blocks_count,
                                            idx_num_total,
                                            idx_to_lit_map_p + idx_to_lit_map_offset,
                                            idx_to_lit_map_size - idx_to_lit_map_offset,
                                            lit_map_p,
                                            literals_num,
                                            lit_id_hash_table_buffer_p,
                                            hash_table_size)
      && (vm_instr_counter_t) instructions_number == instructions_number)
  {
    MEM_CP_SET_NON_NULL_POINTER (header_p->lit_id_hash_cp, lit_id_hash_table_buffer_p);
    header_p->instrs_p = instrs_p;
    header_p->instrs_count = (vm_instr_counter_t) instructions_number;
    MEM_CP_SET_POINTER (header_p->next_header_cp, first_bytecode_header_p);

    first_bytecode_header_p = header_p;

    return header_p;
  }
  else
  {
    mem_heap_free_block (buffer_p);
    return NULL;
  }
} /* serializer_load_bytecode_with_idx_map */

#endif /* JERRY_ENABLE_SNAPSHOT */
