/*
   +----------------------------------------------------------------------+
   | Zend JIT                                                             |
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Dmitry Stogov <dmitry@php.net>                              |
   +----------------------------------------------------------------------+
*/

#include "Zend/zend_API.h"

static ZEND_COLD void undef_result_after_exception() {
	const zend_op *opline = EG(opline_before_exception);
	ZEND_ASSERT(EG(exception));
	if (opline->result_type & (IS_VAR | IS_TMP_VAR)) {
		zend_execute_data *execute_data = EG(current_execute_data);
		ZVAL_UNDEF(EX_VAR(opline->result.var));
	}
}

static zend_never_inline zend_function* ZEND_FASTCALL _zend_jit_init_func_run_time_cache(const zend_op_array *op_array) /* {{{ */
{
	void **run_time_cache;

	run_time_cache = zend_arena_alloc(&CG(arena), op_array->cache_size);
	memset(run_time_cache, 0, op_array->cache_size);
	ZEND_MAP_PTR_SET(op_array->run_time_cache, run_time_cache);
	return (zend_function*)op_array;
}
/* }}} */

static zend_never_inline zend_op_array* ZEND_FASTCALL zend_jit_init_func_run_time_cache_helper(zend_op_array *op_array) /* {{{ */
{
	void **run_time_cache;

	if (!RUN_TIME_CACHE(op_array)) {
		run_time_cache = zend_arena_alloc(&CG(arena), op_array->cache_size);
		memset(run_time_cache, 0, op_array->cache_size);
		ZEND_MAP_PTR_SET(op_array->run_time_cache, run_time_cache);
	}
	return op_array;
}
/* }}} */

static zend_function* ZEND_FASTCALL zend_jit_find_func_helper(zend_string *name)
{
	zval *func = zend_hash_find_ex(EG(function_table), name, 1);
	zend_function *fbc;

	if (UNEXPECTED(func == NULL)) {
		return NULL;
	}
	fbc = Z_FUNC_P(func);
	if (EXPECTED(fbc->type == ZEND_USER_FUNCTION) && UNEXPECTED(!RUN_TIME_CACHE(&fbc->op_array))) {
		fbc = _zend_jit_init_func_run_time_cache(&fbc->op_array);
	}
	return fbc;
}

static zend_function* ZEND_FASTCALL zend_jit_find_ns_func_helper(zval *func_name)
{
	zval *func = zend_hash_find_ex(EG(function_table), Z_STR_P(func_name + 1), 1);
	zend_function *fbc;

	if (func == NULL) {
		func = zend_hash_find_ex(EG(function_table), Z_STR_P(func_name + 2), 1);
		if (UNEXPECTED(func == NULL)) {
			return NULL;
		}
	}
	fbc = Z_FUNC_P(func);
	if (EXPECTED(fbc->type == ZEND_USER_FUNCTION) && UNEXPECTED(!RUN_TIME_CACHE(&fbc->op_array))) {
		fbc = _zend_jit_init_func_run_time_cache(&fbc->op_array);
	}
	return fbc;
}

static zend_execute_data* ZEND_FASTCALL zend_jit_extend_stack_helper(uint32_t used_stack, zend_function *fbc)
{
	zend_execute_data *call = (zend_execute_data*)zend_vm_stack_extend(used_stack);
	call->func = fbc;
	ZEND_CALL_INFO(call) = ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_ALLOCATED;
	return call;
}

static zend_execute_data* ZEND_FASTCALL zend_jit_int_extend_stack_helper(uint32_t used_stack)
{
	zend_execute_data *call = (zend_execute_data*)zend_vm_stack_extend(used_stack);
	ZEND_CALL_INFO(call) = ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_ALLOCATED;
	return call;
}

static zval* ZEND_FASTCALL zend_jit_symtable_find(HashTable *ht, zend_string *str)
{
	zend_ulong idx;
	register const char *tmp = str->val;

	do {
		if (*tmp > '9') {
			break;
		} else if (*tmp < '0') {
			if (*tmp != '-') {
				break;
			}
			tmp++;
			if (*tmp > '9' || *tmp < '0') {
				break;
			}
		}
		if (_zend_handle_numeric_str_ex(str->val, str->len, &idx)) {
			return zend_hash_index_find(ht, idx);
		}
	} while (0);

	return zend_hash_find(ht, str);
}

static zval* ZEND_FASTCALL zend_jit_hash_index_lookup_rw(HashTable *ht, zend_long idx)
{
	zval *retval = _zend_hash_index_find(ht, idx);

	if (!retval) {
		zend_error(E_NOTICE,"Undefined offset: " ZEND_LONG_FMT, idx);
		retval = zend_hash_index_update(ht, idx, &EG(uninitialized_zval));
	}
	return retval;
}

static zval* ZEND_FASTCALL zend_jit_hash_index_lookup_w(HashTable *ht, zend_long idx)
{
	zval *retval = _zend_hash_index_find(ht, idx);

	if (!retval) {
		retval = zend_hash_index_add_new(ht, idx, &EG(uninitialized_zval));
	}
	return retval;
}

static zval* ZEND_FASTCALL zend_jit_hash_lookup_rw(HashTable *ht, zend_string *str)
{
	zval *retval = zend_hash_find(ht, str);

	if (retval) {
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
			retval = Z_INDIRECT_P(retval);
			if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
				zend_error(E_NOTICE,"Undefined index: %s", ZSTR_VAL(str));
				ZVAL_NULL(retval);
			}
		}
	} else {
		zend_error(E_NOTICE,"Undefined index: %s", ZSTR_VAL(str));
		retval = zend_hash_update(ht, str, &EG(uninitialized_zval));
	}
	return retval;
}

static zval* ZEND_FASTCALL zend_jit_hash_lookup_w(HashTable *ht, zend_string *str)
{
	zval *retval = zend_hash_find(ht, str);

	if (retval) {
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
			retval = Z_INDIRECT_P(retval);
			if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
				ZVAL_NULL(retval);
			}
		}
	} else {
		retval = zend_hash_add_new(ht, str, &EG(uninitialized_zval));
	}
	return retval;
}

static zval* ZEND_FASTCALL zend_jit_symtable_lookup_rw(HashTable *ht, zend_string *str)
{
	zend_ulong idx;
	register const char *tmp = str->val;

	do {
		if (*tmp > '9') {
			break;
		} else if (*tmp < '0') {
			if (*tmp != '-') {
				break;
			}
			tmp++;
			if (*tmp > '9' || *tmp < '0') {
				break;
			}
		}
		if (_zend_handle_numeric_str_ex(str->val, str->len, &idx)) {
			zval *retval = zend_hash_index_find(ht, idx);

			if (!retval) {
				zend_error(E_NOTICE,"Undefined index: %s", ZSTR_VAL(str));
				retval = zend_hash_index_update(ht, idx, &EG(uninitialized_zval));
			}
			return retval;
		}
	} while (0);

	return zend_jit_hash_lookup_rw(ht, str);
}

static zval* ZEND_FASTCALL zend_jit_symtable_lookup_w(HashTable *ht, zend_string *str)
{
	zend_ulong idx;
	register const char *tmp = str->val;

	do {
		if (*tmp > '9') {
			break;
		} else if (*tmp < '0') {
			if (*tmp != '-') {
				break;
			}
			tmp++;
			if (*tmp > '9' || *tmp < '0') {
				break;
			}
		}
		if (_zend_handle_numeric_str_ex(str->val, str->len, &idx)) {
			zval *retval = zend_hash_index_find(ht, idx);

			if (!retval) {
				retval = zend_hash_index_add_new(ht, idx, &EG(uninitialized_zval));
			}
			return retval;
		}
	} while (0);

	return zend_jit_hash_lookup_w(ht, str);
}

static void ZEND_FASTCALL zend_jit_undefined_op_helper(uint32_t var)
{
	const zend_execute_data *execute_data = EG(current_execute_data);
	zend_string *cv = EX(func)->op_array.vars[EX_VAR_TO_NUM(var)];

	zend_error(E_WARNING, "Undefined variable $%s", ZSTR_VAL(cv));
}

static void ZEND_FASTCALL zend_jit_fetch_dim_r_helper(zend_array *ht, zval *dim, zval *result)
{
	zend_long hval;
	zend_string *offset_key;
	zval *retval;

	if (Z_TYPE_P(dim) == IS_REFERENCE) {
		dim = Z_REFVAL_P(dim);
	}

	switch (Z_TYPE_P(dim)) {
		case IS_LONG:
			hval = Z_LVAL_P(dim);
			goto num_index;
		case IS_STRING:
			offset_key = Z_STR_P(dim);
			goto str_index;
		case IS_UNDEF:
			zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			/* break missing intentionally */
		case IS_NULL:
			offset_key = ZSTR_EMPTY_ALLOC();
			goto str_index;
		case IS_DOUBLE:
			hval = zend_dval_to_lval(Z_DVAL_P(dim));
			goto num_index;
		case IS_RESOURCE:
			zend_error(E_WARNING, "Resource ID#%d used as offset, casting to integer (%d)", Z_RES_HANDLE_P(dim), Z_RES_HANDLE_P(dim));
			hval = Z_RES_HANDLE_P(dim);
			goto num_index;
		case IS_FALSE:
			hval = 0;
			goto num_index;
		case IS_TRUE:
			hval = 1;
			goto num_index;
		default:
			zend_type_error("Illegal offset type");
			ZVAL_NULL(result);
			return;
	}

str_index:
	retval = zend_hash_find(ht, offset_key);
	if (retval) {
		/* support for $GLOBALS[...] */
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
			retval = Z_INDIRECT_P(retval);
			if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
				zend_error(E_NOTICE, "Undefined index: %s", ZSTR_VAL(offset_key));
				ZVAL_NULL(result);
				return;
			}
		}
	} else {
		zend_error(E_NOTICE, "Undefined index: %s", ZSTR_VAL(offset_key));
		ZVAL_NULL(result);
		return;
	}
	ZVAL_COPY_DEREF(result, retval);
	return;

num_index:
	ZEND_HASH_INDEX_FIND(ht, hval, retval, num_undef);
	ZVAL_COPY_DEREF(result, retval);
	return;

num_undef:
	zend_error(E_NOTICE,"Undefined offset: " ZEND_LONG_FMT, hval);
	ZVAL_NULL(result);
}

static void ZEND_FASTCALL zend_jit_fetch_dim_is_helper(zend_array *ht, zval *dim, zval *result)
{
	zend_long hval;
	zend_string *offset_key;
	zval *retval;

	if (Z_TYPE_P(dim) == IS_REFERENCE) {
		dim = Z_REFVAL_P(dim);
	}

	switch (Z_TYPE_P(dim)) {
		case IS_LONG:
			hval = Z_LVAL_P(dim);
			goto num_index;
		case IS_STRING:
			offset_key = Z_STR_P(dim);
			goto str_index;
		case IS_UNDEF:
			zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			/* break missing intentionally */
		case IS_NULL:
			offset_key = ZSTR_EMPTY_ALLOC();
			goto str_index;
		case IS_DOUBLE:
			hval = zend_dval_to_lval(Z_DVAL_P(dim));
			goto num_index;
		case IS_RESOURCE:
			zend_error(E_WARNING, "Resource ID#%d used as offset, casting to integer (%d)", Z_RES_HANDLE_P(dim), Z_RES_HANDLE_P(dim));
			hval = Z_RES_HANDLE_P(dim);
			goto num_index;
		case IS_FALSE:
			hval = 0;
			goto num_index;
		case IS_TRUE:
			hval = 1;
			goto num_index;
		default:
			zend_type_error("Illegal offset type");
			ZVAL_NULL(result);
			return;
	}

str_index:
	retval = zend_hash_find(ht, offset_key);
	if (retval) {
		/* support for $GLOBALS[...] */
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
			retval = Z_INDIRECT_P(retval);
			if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
				ZVAL_NULL(result);
				return;
			}
		}
	} else {
		ZVAL_NULL(result);
		return;
	}
	ZVAL_COPY_DEREF(result, retval);
	return;

num_index:
	ZEND_HASH_INDEX_FIND(ht, hval, retval, num_undef);
	ZVAL_COPY_DEREF(result, retval);
	return;

num_undef:
	ZVAL_NULL(result);
}

static int ZEND_FASTCALL zend_jit_fetch_dim_isset_helper(zend_array *ht, zval *dim)
{
	zend_long hval;
	zend_string *offset_key;
	zval *retval;

	if (Z_TYPE_P(dim) == IS_REFERENCE) {
		dim = Z_REFVAL_P(dim);
	}

	switch (Z_TYPE_P(dim)) {
		case IS_LONG:
			hval = Z_LVAL_P(dim);
			goto num_index;
		case IS_STRING:
			offset_key = Z_STR_P(dim);
			goto str_index;
		case IS_UNDEF:
			zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			/* break missing intentionally */
		case IS_NULL:
			offset_key = ZSTR_EMPTY_ALLOC();
			goto str_index;
		case IS_DOUBLE:
			hval = zend_dval_to_lval(Z_DVAL_P(dim));
			goto num_index;
		case IS_RESOURCE:
			zend_error(E_WARNING, "Resource ID#%d used as offset, casting to integer (%d)", Z_RES_HANDLE_P(dim), Z_RES_HANDLE_P(dim));
			hval = Z_RES_HANDLE_P(dim);
			goto num_index;
		case IS_FALSE:
			hval = 0;
			goto num_index;
		case IS_TRUE:
			hval = 1;
			goto num_index;
		default:
			zend_type_error("Illegal offset type in isset or empty");
			return 0;
	}

str_index:
	retval = zend_hash_find(ht, offset_key);
	if (retval) {
		/* support for $GLOBALS[...] */
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
			retval = Z_INDIRECT_P(retval);
		}
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_REFERENCE)) {
			retval = Z_REFVAL_P(retval);
		}
		return (Z_TYPE_P(retval) > IS_NULL);
	} else {
		return 0;
	}

num_index:
	ZEND_HASH_INDEX_FIND(ht, hval, retval, num_undef);
	if (UNEXPECTED(Z_TYPE_P(retval) == IS_REFERENCE)) {
		retval = Z_REFVAL_P(retval);
	}
	return (Z_TYPE_P(retval) > IS_NULL);

num_undef:
	return 0;
}

static zval* ZEND_FASTCALL zend_jit_fetch_dim_rw_helper(zend_array *ht, zval *dim)
{
	zend_long hval;
	zend_string *offset_key;
	zval *retval;

	if (Z_TYPE_P(dim) == IS_REFERENCE) {
		dim = Z_REFVAL_P(dim);
	}

	switch (Z_TYPE_P(dim)) {
		case IS_LONG:
			hval = Z_LVAL_P(dim);
			goto num_index;
		case IS_STRING:
			offset_key = Z_STR_P(dim);
			goto str_index;
		case IS_UNDEF:
			zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			/* break missing intentionally */
		case IS_NULL:
			offset_key = ZSTR_EMPTY_ALLOC();
			goto str_index;
		case IS_DOUBLE:
			hval = zend_dval_to_lval(Z_DVAL_P(dim));
			goto num_index;
		case IS_RESOURCE:
			zend_error(E_WARNING, "Resource ID#%d used as offset, casting to integer (%d)", Z_RES_HANDLE_P(dim), Z_RES_HANDLE_P(dim));
			hval = Z_RES_HANDLE_P(dim);
			goto num_index;
		case IS_FALSE:
			hval = 0;
			goto num_index;
		case IS_TRUE:
			hval = 1;
			goto num_index;
		default:
			zend_type_error("Illegal offset type");
			undef_result_after_exception();
			return NULL;
	}

str_index:
	retval = zend_hash_find(ht, offset_key);
	if (retval) {
		/* support for $GLOBALS[...] */
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
			retval = Z_INDIRECT_P(retval);
			if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
				zend_error(E_NOTICE, "Undefined index: %s", ZSTR_VAL(offset_key));
				ZVAL_NULL(retval);
			}
		}
	} else {
		zend_error(E_NOTICE, "Undefined index: %s", ZSTR_VAL(offset_key));
		retval = zend_hash_update(ht, offset_key, &EG(uninitialized_zval));
	}
	return retval;

num_index:
	ZEND_HASH_INDEX_FIND(ht, hval, retval, num_undef);
	return retval;

num_undef:
	zend_error(E_NOTICE,"Undefined offset: " ZEND_LONG_FMT, hval);
	retval = zend_hash_index_update(ht, hval, &EG(uninitialized_zval));
	return retval;
}

static zval* ZEND_FASTCALL zend_jit_fetch_dim_w_helper(zend_array *ht, zval *dim)
{
	zend_long hval;
	zend_string *offset_key;
	zval *retval;

	if (Z_TYPE_P(dim) == IS_REFERENCE) {
		dim = Z_REFVAL_P(dim);
	}

	switch (Z_TYPE_P(dim)) {
		case IS_LONG:
			hval = Z_LVAL_P(dim);
			goto num_index;
		case IS_STRING:
			offset_key = Z_STR_P(dim);
			goto str_index;
		case IS_UNDEF:
			zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			/* break missing intentionally */
		case IS_NULL:
			offset_key = ZSTR_EMPTY_ALLOC();
			goto str_index;
		case IS_DOUBLE:
			hval = zend_dval_to_lval(Z_DVAL_P(dim));
			goto num_index;
		case IS_RESOURCE:
			zend_error(E_WARNING, "Resource ID#%d used as offset, casting to integer (%d)", Z_RES_HANDLE_P(dim), Z_RES_HANDLE_P(dim));
			hval = Z_RES_HANDLE_P(dim);
			goto num_index;
		case IS_FALSE:
			hval = 0;
			goto num_index;
		case IS_TRUE:
			hval = 1;
			goto num_index;
		default:
			zend_type_error("Illegal offset type");
			undef_result_after_exception();
			return NULL;
	}

str_index:
	retval = zend_hash_find(ht, offset_key);
	if (retval) {
		/* support for $GLOBALS[...] */
		if (UNEXPECTED(Z_TYPE_P(retval) == IS_INDIRECT)) {
			retval = Z_INDIRECT_P(retval);
			if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
				ZVAL_NULL(retval);
			}
		}
	} else {
		retval = zend_hash_add_new(ht, offset_key, &EG(uninitialized_zval));
	}
	return retval;

num_index:
	ZEND_HASH_INDEX_FIND(ht, hval, retval, num_undef);
	return retval;

num_undef:
	retval = zend_hash_index_add_new(ht, hval, &EG(uninitialized_zval));
	return retval;
}

static void ZEND_FASTCALL zend_jit_fetch_dim_str_r_helper(zval *container, zval *dim, zval *result)
{
	zend_long offset;

try_string_offset:
	if (UNEXPECTED(Z_TYPE_P(dim) != IS_LONG)) {
		switch (Z_TYPE_P(dim)) {
			/* case IS_LONG: */
			case IS_STRING:
				if (IS_LONG == is_numeric_string(Z_STRVAL_P(dim), Z_STRLEN_P(dim), NULL, NULL, -1)) {
					break;
				}
				zend_error(E_WARNING, "Illegal string offset '%s'", Z_STRVAL_P(dim));
				break;
			case IS_UNDEF:
				zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			case IS_DOUBLE:
			case IS_NULL:
			case IS_FALSE:
			case IS_TRUE:
				zend_error(E_WARNING, "String offset cast occurred");
				break;
			case IS_REFERENCE:
				dim = Z_REFVAL_P(dim);
				goto try_string_offset;
			default:
				zend_type_error("Illegal offset type");
				break;
		}

		offset = _zval_get_long_func(dim);
	} else {
		offset = Z_LVAL_P(dim);
	}

	if (UNEXPECTED(Z_STRLEN_P(container) < ((offset < 0) ? -(size_t)offset : ((size_t)offset + 1)))) {
		zend_error(E_WARNING, "Uninitialized string offset: " ZEND_LONG_FMT, offset);
		ZVAL_EMPTY_STRING(result);
	} else {
		zend_uchar c;
		zend_long real_offset;

		real_offset = (UNEXPECTED(offset < 0)) /* Handle negative offset */
			? (zend_long)Z_STRLEN_P(container) + offset : offset;
		c = (zend_uchar)Z_STRVAL_P(container)[real_offset];
		ZVAL_CHAR(result, c);
	}
}

static void ZEND_FASTCALL zend_jit_fetch_dim_str_is_helper(zval *container, zval *dim, zval *result)
{
	zend_long offset;

try_string_offset:
	if (UNEXPECTED(Z_TYPE_P(dim) != IS_LONG)) {
		switch (Z_TYPE_P(dim)) {
			/* case IS_LONG: */
			case IS_STRING:
				if (IS_LONG == is_numeric_string(Z_STRVAL_P(dim), Z_STRLEN_P(dim), NULL, NULL, -1)) {
					break;
				}
				ZVAL_NULL(result);
				return;
			case IS_UNDEF:
				zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			case IS_DOUBLE:
			case IS_NULL:
			case IS_FALSE:
			case IS_TRUE:
				break;
			case IS_REFERENCE:
				dim = Z_REFVAL_P(dim);
				goto try_string_offset;
			default:
				zend_type_error("Illegal offset type");
				break;
		}

		offset = _zval_get_long_func(dim);
	} else {
		offset = Z_LVAL_P(dim);
	}

	if (UNEXPECTED(Z_STRLEN_P(container) < ((offset < 0) ? -(size_t)offset : ((size_t)offset + 1)))) {
		ZVAL_NULL(result);
	} else {
		zend_uchar c;
		zend_long real_offset;

		real_offset = (UNEXPECTED(offset < 0)) /* Handle negative offset */
			? (zend_long)Z_STRLEN_P(container) + offset : offset;
		c = (zend_uchar)Z_STRVAL_P(container)[real_offset];
		ZVAL_CHAR(result, c);
	}
}

static void ZEND_FASTCALL zend_jit_fetch_dim_obj_r_helper(zval *container, zval *dim, zval *result)
{
	zval *retval;

	if (UNEXPECTED(Z_TYPE_P(dim) == IS_UNDEF)) {
		zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
		dim = &EG(uninitialized_zval);
	}

	retval = Z_OBJ_HT_P(container)->read_dimension(Z_OBJ_P(container), dim, BP_VAR_R, result);

	if (retval) {
		if (result != retval) {
			ZVAL_COPY_DEREF(result, retval);
		} else if (UNEXPECTED(Z_ISREF_P(retval))) {
			zend_unwrap_reference(retval);
		}
	} else {
		ZVAL_NULL(result);
	}
}

static void ZEND_FASTCALL zend_jit_fetch_dim_obj_is_helper(zval *container, zval *dim, zval *result)
{
	zval *retval;

	if (UNEXPECTED(Z_TYPE_P(dim) == IS_UNDEF)) {
		zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
		dim = &EG(uninitialized_zval);
	}

	retval = Z_OBJ_HT_P(container)->read_dimension(Z_OBJ_P(container), dim, BP_VAR_IS, result);

	if (retval) {
		if (result != retval) {
			ZVAL_COPY_DEREF(result, retval);
		} else if (UNEXPECTED(Z_ISREF_P(retval))) {
			zend_unwrap_reference(result);
		}
	} else {
		ZVAL_NULL(result);
	}
}

static zval* ZEND_FASTCALL zend_jit_fetch_dimension_rw_long_helper(HashTable *ht, zend_long hval)
{
	zend_error(E_NOTICE,"Undefined offset: " ZEND_LONG_FMT, hval);
	return zend_hash_index_update(ht, hval, &EG(uninitialized_zval));
}

static zend_never_inline zend_long zend_check_string_offset(zval *dim, int type)
{
	zend_long offset;

try_again:
	if (UNEXPECTED(Z_TYPE_P(dim) != IS_LONG)) {
		switch(Z_TYPE_P(dim)) {
			case IS_STRING:
				if (IS_LONG == is_numeric_string(Z_STRVAL_P(dim), Z_STRLEN_P(dim), NULL, NULL, -1)) {
					break;
				}
				if (type != BP_VAR_UNSET) {
					zend_error(E_WARNING, "Illegal string offset '%s'", Z_STRVAL_P(dim));
				}
				break;
			case IS_UNDEF:
				zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
			case IS_DOUBLE:
			case IS_NULL:
			case IS_FALSE:
			case IS_TRUE:
				zend_error(E_WARNING, "String offset cast occurred");
				break;
			case IS_REFERENCE:
				dim = Z_REFVAL_P(dim);
				goto try_again;
			default:
				zend_type_error("Illegal offset type");
				break;
		}

		offset = _zval_get_long_func(dim);
	} else {
		offset = Z_LVAL_P(dim);
	}

	return offset;
}

static zend_never_inline ZEND_COLD void zend_wrong_string_offset(void)
{
	const char *msg = NULL;
	const zend_op *opline = EG(current_execute_data)->opline;
	const zend_op *end;
	uint32_t var;

	switch (opline->opcode) {
		case ZEND_ASSIGN_OP:
		case ZEND_ASSIGN_DIM_OP:
		case ZEND_ASSIGN_OBJ_OP:
		case ZEND_ASSIGN_STATIC_PROP_OP:
			msg = "Cannot use assign-op operators with string offsets";
			break;
		case ZEND_FETCH_DIM_W:
		case ZEND_FETCH_DIM_RW:
		case ZEND_FETCH_DIM_FUNC_ARG:
		case ZEND_FETCH_DIM_UNSET:
			/* TODO: Encode the "reason" into opline->extended_value??? */
			var = opline->result.var;
			opline++;
			end = EG(current_execute_data)->func->op_array.opcodes +
				EG(current_execute_data)->func->op_array.last;
			while (opline < end) {
				if (opline->op1_type == IS_VAR && opline->op1.var == var) {
					switch (opline->opcode) {
						case ZEND_ASSIGN_OBJ_OP:
							msg = "Cannot use string offset as an object";
							break;
						case ZEND_ASSIGN_DIM_OP:
							msg = "Cannot use string offset as an array";
							break;
						case ZEND_ASSIGN_OP:
						case ZEND_ASSIGN_STATIC_PROP_OP:
							msg = "Cannot use assign-op operators with string offsets";
							break;
						case ZEND_PRE_INC_OBJ:
						case ZEND_PRE_DEC_OBJ:
						case ZEND_POST_INC_OBJ:
						case ZEND_POST_DEC_OBJ:
						case ZEND_PRE_INC:
						case ZEND_PRE_DEC:
						case ZEND_POST_INC:
						case ZEND_POST_DEC:
							msg = "Cannot increment/decrement string offsets";
							break;
						case ZEND_FETCH_DIM_W:
						case ZEND_FETCH_DIM_RW:
						case ZEND_FETCH_DIM_FUNC_ARG:
						case ZEND_FETCH_DIM_UNSET:
						case ZEND_ASSIGN_DIM:
							msg = "Cannot use string offset as an array";
							break;
						case ZEND_FETCH_OBJ_W:
						case ZEND_FETCH_OBJ_RW:
						case ZEND_FETCH_OBJ_FUNC_ARG:
						case ZEND_FETCH_OBJ_UNSET:
						case ZEND_ASSIGN_OBJ:
							msg = "Cannot use string offset as an object";
							break;
						case ZEND_ASSIGN_REF:
						case ZEND_ADD_ARRAY_ELEMENT:
						case ZEND_INIT_ARRAY:
						case ZEND_MAKE_REF:
							msg = "Cannot create references to/from string offsets";
							break;
						case ZEND_RETURN_BY_REF:
						case ZEND_VERIFY_RETURN_TYPE:
							msg = "Cannot return string offsets by reference";
							break;
						case ZEND_UNSET_DIM:
						case ZEND_UNSET_OBJ:
							msg = "Cannot unset string offsets";
							break;
						case ZEND_YIELD:
							msg = "Cannot yield string offsets by reference";
							break;
						case ZEND_SEND_REF:
						case ZEND_SEND_VAR_EX:
						case ZEND_SEND_FUNC_ARG:
							msg = "Only variables can be passed by reference";
							break;
						EMPTY_SWITCH_DEFAULT_CASE();
					}
					break;
				}
				if (opline->op2_type == IS_VAR && opline->op2.var == var) {
					ZEND_ASSERT(opline->opcode == ZEND_ASSIGN_REF);
					msg = "Cannot create references to/from string offsets";
					break;
				}
			}
			break;
		EMPTY_SWITCH_DEFAULT_CASE();
	}
	ZEND_ASSERT(msg != NULL);
	zend_throw_error(NULL, "%s", msg);
}

static zend_never_inline void zend_assign_to_string_offset(zval *str, zval *dim, zval *value, zval *result)
{
	zend_string *old_str;
	zend_uchar c;
	size_t string_len;
	zend_long offset;

	offset = zend_check_string_offset(dim, BP_VAR_W);
	if (offset < -(zend_long)Z_STRLEN_P(str)) {
		/* Error on negative offset */
		zend_error(E_WARNING, "Illegal string offset:  " ZEND_LONG_FMT, offset);
		if (result) {
			ZVAL_NULL(result);
		}
		return;
	}

	if (Z_TYPE_P(value) != IS_STRING) {
		/* Convert to string, just the time to pick the 1st byte */
		zend_string *tmp = zval_try_get_string_func(value);

		if (UNEXPECTED(!tmp)) {
			if (result) {
				ZVAL_UNDEF(result);
			}
			return;
		}

		string_len = ZSTR_LEN(tmp);
		c = (zend_uchar)ZSTR_VAL(tmp)[0];
		zend_string_release(tmp);
	} else {
		string_len = Z_STRLEN_P(value);
		c = (zend_uchar)Z_STRVAL_P(value)[0];
	}


	if (string_len != 1) {
		if (string_len == 0) {
			/* Error on empty input string */
			zend_throw_error(NULL, "Cannot assign an empty string to a string offset");
			if (result) {
				ZVAL_NULL(result);
			}
			return;
		}

		zend_error(E_WARNING, "Only the first byte will be assigned to the string offset");
	}

	if (offset < 0) { /* Handle negative offset */
		offset += (zend_long)Z_STRLEN_P(str);
	}

	if ((size_t)offset >= Z_STRLEN_P(str)) {
		/* Extend string if needed */
		zend_long old_len = Z_STRLEN_P(str);
		Z_STR_P(str) = zend_string_extend(Z_STR_P(str), offset + 1, 0);
		Z_TYPE_INFO_P(str) = IS_STRING_EX;
		memset(Z_STRVAL_P(str) + old_len, ' ', offset - old_len);
		Z_STRVAL_P(str)[offset+1] = 0;
	} else if (!Z_REFCOUNTED_P(str)) {
		old_str = Z_STR_P(str);
		Z_STR_P(str) = zend_string_init(Z_STRVAL_P(str), Z_STRLEN_P(str), 0);
		Z_TYPE_INFO_P(str) = IS_STRING_EX;
		zend_string_release(old_str);
	} else {
		SEPARATE_STRING(str);
		zend_string_forget_hash_val(Z_STR_P(str));
	}

	Z_STRVAL_P(str)[offset] = c;

	if (result) {
		/* Return the new character */
		ZVAL_CHAR(result, c);
	}
}

static void ZEND_FASTCALL zend_jit_assign_dim_helper(zval *object_ptr, zval *dim, zval *value, zval *result)
{
	if (EXPECTED(Z_TYPE_P(object_ptr) == IS_OBJECT)) {
		ZVAL_DEREF(value);
		Z_OBJ_HT_P(object_ptr)->write_dimension(Z_OBJ_P(object_ptr), dim, value);
		if (result) {
			if (EXPECTED(!EG(exception))) {
				ZVAL_COPY(result, value);
			} else {
				ZVAL_UNDEF(result);
			}
		}
	} else if (EXPECTED(Z_TYPE_P(object_ptr) == IS_STRING)) {
		if (!dim) {
			zend_throw_error(NULL, "[] operator not supported for strings");
			if (result) {
				ZVAL_UNDEF(result);
			}
		} else {
			zend_assign_to_string_offset(object_ptr, dim, value, result);
		}
	} else {
		zend_throw_error(NULL, "Cannot use a scalar value as an array");
		if (result) {
			ZVAL_UNDEF(result);
		}
	}
}

static void ZEND_FASTCALL zend_jit_assign_dim_op_helper(zval *container, zval *dim, zval *value, binary_op_type binary_op)
{
	if (EXPECTED(Z_TYPE_P(container) == IS_OBJECT)) {
		zval *object = container;
		zval *property = dim;
		zval *z;
		zval rv, res;

		z = Z_OBJ_HT_P(object)->read_dimension(Z_OBJ_P(object), property, BP_VAR_R, &rv);
		if (z != NULL) {

			if (binary_op(&res, Z_ISREF_P(z) ? Z_REFVAL_P(z) : z, value) == SUCCESS) {
				Z_OBJ_HT_P(object)->write_dimension(Z_OBJ_P(object), property, &res);
			}
			if (z == &rv) {
				zval_ptr_dtor(&rv);
			}
//???			if (retval) {
//???				ZVAL_COPY(retval, &res);
//???			}
			zval_ptr_dtor(&res);
		} else {
			zend_error(E_WARNING, "Attempt to assign property of non-object");
//???			if (retval) {
//???				ZVAL_NULL(retval);
//???			}
		}
	} else {
		if (UNEXPECTED(Z_TYPE_P(container) == IS_STRING)) {
			if (!dim) {
				zend_throw_error(NULL, "[] operator not supported for strings");
			} else {
				zend_check_string_offset(dim, BP_VAR_RW);
				zend_wrong_string_offset();
			}
//???		} else if (EXPECTED(Z_TYPE_P(container) <= IS_FALSE)) {
//???			ZEND_VM_C_GOTO(assign_dim_op_convert_to_array);
		} else {
			zend_throw_error(NULL, "Cannot use a scalar value as an array");
//???			if (retval) {
//???				ZVAL_NULL(retval);
//???			}
		}
	}
}

static void ZEND_FASTCALL zend_jit_fast_assign_concat_helper(zval *op1, zval *op2)
{
	size_t op1_len = Z_STRLEN_P(op1);
	size_t op2_len = Z_STRLEN_P(op2);
	size_t result_len = op1_len + op2_len;
	zend_string *result_str;

	if (UNEXPECTED(op1_len > SIZE_MAX - op2_len)) {
		zend_throw_error(NULL, "String size overflow");
		return;
	}

	if (Z_REFCOUNTED_P(op1)) {
		result_str = zend_string_extend(Z_STR_P(op1), result_len, 0);
	} else {
		result_str = zend_string_alloc(result_len, 0);
		memcpy(ZSTR_VAL(result_str), Z_STRVAL_P(op1), op1_len);
	}

	ZVAL_NEW_STR(op1, result_str);

	memcpy(ZSTR_VAL(result_str) + op1_len, Z_STRVAL_P(op2), op2_len);
	ZSTR_VAL(result_str)[result_len] = '\0';
}

static void ZEND_FASTCALL zend_jit_fast_concat_helper(zval *result, zval *op1, zval *op2)
{
	size_t op1_len = Z_STRLEN_P(op1);
	size_t op2_len = Z_STRLEN_P(op2);
	size_t result_len = op1_len + op2_len;
	zend_string *result_str;

	if (UNEXPECTED(op1_len > SIZE_MAX - op2_len)) {
		zend_throw_error(NULL, "String size overflow");
		return;
	}

	result_str = zend_string_alloc(result_len, 0);
	memcpy(ZSTR_VAL(result_str), Z_STRVAL_P(op1), op1_len);

	ZVAL_NEW_STR(result, result_str);

	memcpy(ZSTR_VAL(result_str) + op1_len, Z_STRVAL_P(op2), op2_len);
	ZSTR_VAL(result_str)[result_len] = '\0';
}

static int ZEND_FASTCALL zend_jit_isset_dim_helper(zval *container, zval *offset)
{
	if (UNEXPECTED(Z_TYPE_P(offset) == IS_UNDEF)) {
		zend_jit_undefined_op_helper(EG(current_execute_data)->opline->op2.var);
		offset = &EG(uninitialized_zval);
	}

	if (EXPECTED(Z_TYPE_P(container) == IS_OBJECT)) {
		return Z_OBJ_HT_P(container)->has_dimension(Z_OBJ_P(container), offset, 0);
	} else if (EXPECTED(Z_TYPE_P(container) == IS_STRING)) { /* string offsets */
		zend_long lval;

		if (EXPECTED(Z_TYPE_P(offset) == IS_LONG)) {
			lval = Z_LVAL_P(offset);
isset_str_offset:
			if (UNEXPECTED(lval < 0)) { /* Handle negative offset */
				lval += (zend_long)Z_STRLEN_P(container);
			}
			if (EXPECTED(lval >= 0) && (size_t)lval < Z_STRLEN_P(container)) {
				return 1;
			}
		} else {
			ZVAL_DEREF(offset);
			if (Z_TYPE_P(offset) < IS_STRING /* simple scalar types */
					|| (Z_TYPE_P(offset) == IS_STRING /* or numeric string */
						&& IS_LONG == is_numeric_string(Z_STRVAL_P(offset), Z_STRLEN_P(offset), NULL, NULL, 0))) {
				lval = zval_get_long(offset);
				goto isset_str_offset;
			}
		}
	}
	return 0;
}

static void ZEND_FASTCALL zend_jit_free_call_frame(zend_execute_data *call)
{
	zend_vm_stack_free_call_frame(call);
}

static zend_reference* ZEND_FASTCALL zend_jit_fetch_global_helper(zend_string *varname, void **cache_slot)
{
	zval *value;
	uintptr_t idx;
	zend_reference *ref;

	/* We store "hash slot index" + 1 (NULL is a mark of uninitialized cache slot) */
	idx = (uintptr_t)CACHED_PTR_EX(cache_slot) - 1;
	if (EXPECTED(idx < EG(symbol_table).nNumUsed * sizeof(Bucket))) {
		Bucket *p = (Bucket*)((char*)EG(symbol_table).arData + idx);

		if (EXPECTED(Z_TYPE(p->val) != IS_UNDEF) &&
	        (EXPECTED(p->key == varname) ||
	         (EXPECTED(p->h == ZSTR_H(varname)) &&
	          EXPECTED(p->key != NULL) &&
	          EXPECTED(zend_string_equal_content(p->key, varname))))) {

			value = (zval*)p; /* value = &p->val; */
			goto check_indirect;
		}
	}

	value = zend_hash_find_ex(&EG(symbol_table), varname, 1);
	if (UNEXPECTED(value == NULL)) {
		value = zend_hash_add_new(&EG(symbol_table), varname, &EG(uninitialized_zval));
		idx = (char*)value - (char*)EG(symbol_table).arData;
		/* Store "hash slot index" + 1 (NULL is a mark of uninitialized cache slot) */
		CACHE_PTR_EX(cache_slot, (void*)(idx + 1));
	} else {
		idx = (char*)value - (char*)EG(symbol_table).arData;
		/* Store "hash slot index" + 1 (NULL is a mark of uninitialized cache slot) */
		CACHE_PTR_EX(cache_slot, (void*)(idx + 1));
check_indirect:
		/* GLOBAL variable may be an INDIRECT pointer to CV */
		if (UNEXPECTED(Z_TYPE_P(value) == IS_INDIRECT)) {
			value = Z_INDIRECT_P(value);
			if (UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
				ZVAL_NULL(value);
			}
		}
	}

	if (UNEXPECTED(!Z_ISREF_P(value))) {
		ZVAL_MAKE_REF_EX(value, 2);
		ref = Z_REF_P(value);
	} else {
		ref = Z_REF_P(value);
		GC_ADDREF(ref);
	}

	return ref;
}

static zend_always_inline zend_bool zend_jit_verify_type_common(zval *arg, zend_arg_info *arg_info, void **cache_slot)
{
	uint32_t type_mask;

	if (ZEND_TYPE_HAS_CLASS(arg_info->type) && Z_TYPE_P(arg) == IS_OBJECT) {
		zend_class_entry *ce;
		if (ZEND_TYPE_HAS_LIST(arg_info->type)) {
			zend_type *list_type;
			ZEND_TYPE_LIST_FOREACH(ZEND_TYPE_LIST(arg_info->type), list_type) {
				if (*cache_slot) {
					ce = *cache_slot;
				} else {
					ce = zend_fetch_class(ZEND_TYPE_NAME(*list_type),
						(ZEND_FETCH_CLASS_AUTO | ZEND_FETCH_CLASS_NO_AUTOLOAD));
					if (!ce) {
						cache_slot++;
						continue;
					}
					*cache_slot = ce;
				}
				if (instanceof_function(Z_OBJCE_P(arg), ce)) {
					return 1;
				}
				cache_slot++;
			} ZEND_TYPE_LIST_FOREACH_END();
		} else {
			if (EXPECTED(*cache_slot)) {
				ce = (zend_class_entry *) *cache_slot;
			} else {
				ce = zend_fetch_class(ZEND_TYPE_NAME(arg_info->type), (ZEND_FETCH_CLASS_AUTO | ZEND_FETCH_CLASS_NO_AUTOLOAD));
				if (UNEXPECTED(!ce)) {
					goto builtin_types;
				}
				*cache_slot = (void *) ce;
			}
			if (instanceof_function(Z_OBJCE_P(arg), ce)) {
				return 1;
			}
		}
	}

builtin_types:
	type_mask = ZEND_TYPE_FULL_MASK(arg_info->type);
	if ((type_mask & MAY_BE_CALLABLE) && zend_is_callable(arg, 0, NULL)) {
		return 1;
	}
	if ((type_mask & MAY_BE_ITERABLE) && zend_is_iterable(arg)) {
		return 1;
	}
	if ((type_mask & MAY_BE_STATIC) && zend_value_instanceof_static(arg)) {
		return 1;
	}
	if (zend_verify_scalar_type_hint(type_mask, arg, ZEND_ARG_USES_STRICT_TYPES(), /* is_internal */ 0)) {
		return 1;
	}
	return 0;
}

static zend_bool ZEND_FASTCALL zend_jit_verify_arg_slow(zval *arg, zend_arg_info *arg_info)
{
	zend_execute_data *execute_data = EG(current_execute_data);
	const zend_op *opline = EX(opline);
	void **cache_slot = CACHE_ADDR(opline->extended_value);
	zend_bool ret;

	ret = zend_jit_verify_type_common(arg, arg_info, cache_slot);
	if (UNEXPECTED(!ret)) {
		zend_verify_arg_error(EX(func), arg_info, opline->op1.num, cache_slot, arg);
		return 0;
	}
	return ret;
}

static void ZEND_FASTCALL zend_jit_verify_return_slow(zval *arg, const zend_op_array *op_array, zend_arg_info *arg_info, void **cache_slot)
{
	if (UNEXPECTED(!zend_jit_verify_type_common(arg, arg_info, cache_slot))) {
		zend_verify_return_error((zend_function*)op_array, cache_slot, arg);
	}
}

static void ZEND_FASTCALL zend_jit_fetch_obj_r_slow(zend_object *zobj)
{
	zval *retval;
	zend_execute_data *execute_data = EG(current_execute_data);
	const zend_op *opline = EX(opline);
	zend_string *name = Z_STR_P(RT_CONSTANT(opline, opline->op2));
	zval *result = EX_VAR(opline->result.var);
	void **cache_slot = CACHE_ADDR(opline->extended_value & ~ZEND_FETCH_OBJ_FLAGS);

	retval = zobj->handlers->read_property(zobj, name, BP_VAR_R, cache_slot, result);
	if (retval != result) {
		ZVAL_COPY_DEREF(result, retval);
	} else if (UNEXPECTED(Z_ISREF_P(retval))) {
		zend_unwrap_reference(retval);
	}
}

static void ZEND_FASTCALL zend_jit_fetch_obj_r_dynamic(zend_object *zobj, intptr_t prop_offset)
{
	if (zobj->properties) {
		zval *retval;
		zend_execute_data *execute_data = EG(current_execute_data);
		const zend_op *opline = EX(opline);
		zend_string *name = Z_STR_P(RT_CONSTANT(opline, opline->op2));
		zval *result = EX_VAR(opline->result.var);
		void **cache_slot = CACHE_ADDR(opline->extended_value & ~ZEND_FETCH_OBJ_FLAGS);

		if (!IS_UNKNOWN_DYNAMIC_PROPERTY_OFFSET(prop_offset)) {
			intptr_t idx = ZEND_DECODE_DYN_PROP_OFFSET(prop_offset);

			if (EXPECTED(idx < zobj->properties->nNumUsed * sizeof(Bucket))) {
				Bucket *p = (Bucket*)((char*)zobj->properties->arData + idx);

				if (EXPECTED(Z_TYPE(p->val) != IS_UNDEF) &&
			        (EXPECTED(p->key == name) ||
			         (EXPECTED(p->h == ZSTR_H(name)) &&
			          EXPECTED(p->key != NULL) &&
			          EXPECTED(ZSTR_LEN(p->key) == ZSTR_LEN(name)) &&
			          EXPECTED(memcmp(ZSTR_VAL(p->key), ZSTR_VAL(name), ZSTR_LEN(name)) == 0)))) {
					ZVAL_COPY_DEREF(result, &p->val);
					return;
				}
			}
			CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_DYNAMIC_PROPERTY_OFFSET);
		}

		retval = zend_hash_find_ex(zobj->properties, name, 1);

		if (EXPECTED(retval)) {
			intptr_t idx = (char*)retval - (char*)zobj->properties->arData;
			CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_ENCODE_DYN_PROP_OFFSET(idx));
			ZVAL_COPY_DEREF(result, retval);
			return;
		}
	}
	zend_jit_fetch_obj_r_slow(zobj);
}

static void ZEND_FASTCALL zend_jit_fetch_obj_is_slow(zend_object *zobj)
{
	zval *retval;
	zend_execute_data *execute_data = EG(current_execute_data);
	const zend_op *opline = EX(opline);
	zend_string *name = Z_STR_P(RT_CONSTANT(opline, opline->op2));
	zval *result = EX_VAR(opline->result.var);
	void **cache_slot = CACHE_ADDR(opline->extended_value & ~ZEND_FETCH_OBJ_FLAGS);

	retval = zobj->handlers->read_property(zobj, name, BP_VAR_IS, cache_slot, result);
	if (retval != result) {
		ZVAL_COPY_DEREF(result, retval);
	} else if (UNEXPECTED(Z_ISREF_P(retval))) {
		zend_unwrap_reference(retval);
	}
}

static void ZEND_FASTCALL zend_jit_fetch_obj_is_dynamic(zend_object *zobj, intptr_t prop_offset)
{
	if (zobj->properties) {
		zval *retval;
		zend_execute_data *execute_data = EG(current_execute_data);
		const zend_op *opline = EX(opline);
		zend_string *name = Z_STR_P(RT_CONSTANT(opline, opline->op2));
		zval *result = EX_VAR(opline->result.var);
		void **cache_slot = CACHE_ADDR(opline->extended_value & ~ZEND_FETCH_OBJ_FLAGS);

		if (!IS_UNKNOWN_DYNAMIC_PROPERTY_OFFSET(prop_offset)) {
			intptr_t idx = ZEND_DECODE_DYN_PROP_OFFSET(prop_offset);

			if (EXPECTED(idx < zobj->properties->nNumUsed * sizeof(Bucket))) {
				Bucket *p = (Bucket*)((char*)zobj->properties->arData + idx);

				if (EXPECTED(Z_TYPE(p->val) != IS_UNDEF) &&
			        (EXPECTED(p->key == name) ||
			         (EXPECTED(p->h == ZSTR_H(name)) &&
			          EXPECTED(p->key != NULL) &&
			          EXPECTED(ZSTR_LEN(p->key) == ZSTR_LEN(name)) &&
			          EXPECTED(memcmp(ZSTR_VAL(p->key), ZSTR_VAL(name), ZSTR_LEN(name)) == 0)))) {
					ZVAL_COPY_DEREF(result, &p->val);
					return;
				}
			}
			CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_DYNAMIC_PROPERTY_OFFSET);
		}

		retval = zend_hash_find_ex(zobj->properties, name, 1);

		if (EXPECTED(retval)) {
			intptr_t idx = (char*)retval - (char*)zobj->properties->arData;
			CACHE_PTR_EX(cache_slot + 1, (void*)ZEND_ENCODE_DYN_PROP_OFFSET(idx));
			ZVAL_COPY(result, retval);
			return;
		}
	}
	zend_jit_fetch_obj_is_slow(zobj);
}

static zend_always_inline zend_bool promotes_to_array(zval *val) {
	return Z_TYPE_P(val) <= IS_FALSE
		|| (Z_ISREF_P(val) && Z_TYPE_P(Z_REFVAL_P(val)) <= IS_FALSE);
}

static zend_always_inline zend_bool check_type_array_assignable(zend_type type) {
	if (!ZEND_TYPE_IS_SET(type)) {
		return 1;
	}
	return (ZEND_TYPE_FULL_MASK(type) & (MAY_BE_ITERABLE|MAY_BE_ARRAY)) != 0;
}

static zend_property_info *zend_object_fetch_property_type_info(
		zend_object *obj, zval *slot)
{
	if (EXPECTED(!ZEND_CLASS_HAS_TYPE_HINTS(obj->ce))) {
		return NULL;
	}

	/* Not a declared property */
	if (UNEXPECTED(slot < obj->properties_table ||
			slot >= obj->properties_table + obj->ce->default_properties_count)) {
		return NULL;
	}

	return zend_get_typed_property_info_for_slot(obj, slot);
}

static zend_never_inline ZEND_COLD void zend_throw_auto_init_in_prop_error(zend_property_info *prop, const char *type) {
	zend_string *type_str = zend_type_to_string(prop->type);
	zend_type_error(
		"Cannot auto-initialize an %s inside property %s::$%s of type %s",
		type,
		ZSTR_VAL(prop->ce->name), zend_get_unmangled_property_name(prop->name),
		ZSTR_VAL(type_str)
	);
	zend_string_release(type_str);
}

static zend_never_inline ZEND_COLD void zend_throw_access_uninit_prop_by_ref_error(
		zend_property_info *prop) {
	zend_throw_error(NULL,
		"Cannot access uninitialized non-nullable property %s::$%s by reference",
		ZSTR_VAL(prop->ce->name),
		zend_get_unmangled_property_name(prop->name));
}

static zend_never_inline zend_bool zend_handle_fetch_obj_flags(
		zval *result, zval *ptr, zend_object *obj, zend_property_info *prop_info, uint32_t flags)
{
	switch (flags) {
		case ZEND_FETCH_DIM_WRITE:
			if (promotes_to_array(ptr)) {
				if (!prop_info) {
					prop_info = zend_object_fetch_property_type_info(obj, ptr);
					if (!prop_info) {
						break;
					}
				}
				if (!check_type_array_assignable(prop_info->type)) {
					zend_throw_auto_init_in_prop_error(prop_info, "array");
					if (result) ZVAL_ERROR(result);
					return 0;
				}
			}
			break;
		case ZEND_FETCH_REF:
			if (Z_TYPE_P(ptr) != IS_REFERENCE) {
				if (!prop_info) {
					prop_info = zend_object_fetch_property_type_info(obj, ptr);
					if (!prop_info) {
						break;
					}
				}
				if (Z_TYPE_P(ptr) == IS_UNDEF) {
					if (!ZEND_TYPE_ALLOW_NULL(prop_info->type)) {
						zend_throw_access_uninit_prop_by_ref_error(prop_info);
						if (result) ZVAL_ERROR(result);
						return 0;
					}
					ZVAL_NULL(ptr);
				}

				ZVAL_NEW_REF(ptr, ptr);
				ZEND_REF_ADD_TYPE_SOURCE(Z_REF_P(ptr), prop_info);
			}
			break;
		EMPTY_SWITCH_DEFAULT_CASE()
	}
	return 1;
}

static void ZEND_FASTCALL zend_jit_fetch_obj_w_slow(zend_object *zobj)
{
	zval *retval;
	zend_execute_data *execute_data = EG(current_execute_data);
	const zend_op *opline = EX(opline);
	zend_string *name = Z_STR_P(RT_CONSTANT(opline, opline->op2));
	zval *result = EX_VAR(opline->result.var);
	void **cache_slot = CACHE_ADDR(opline->extended_value & ~ZEND_FETCH_OBJ_FLAGS);

	retval = zobj->handlers->get_property_ptr_ptr(zobj, name, BP_VAR_W, cache_slot);
	if (NULL == retval) {
		retval = zobj->handlers->read_property(zobj, name, BP_VAR_W, cache_slot, result);
		if (retval == result) {
			if (UNEXPECTED(Z_ISREF_P(retval) && Z_REFCOUNT_P(retval) == 1)) {
				ZVAL_UNREF(retval);
			}
			return;
		}
	} else if (UNEXPECTED(Z_ISERROR_P(retval))) {
		ZVAL_ERROR(result);
		return;
	}

	ZVAL_INDIRECT(result, retval);

	/* Support for typed properties */
	do {
		uint32_t flags = opline->extended_value & ZEND_FETCH_OBJ_FLAGS;

		if (flags) {
			zend_property_info *prop_info = NULL;

			if (opline->op2_type == IS_CONST) {
				prop_info = CACHED_PTR_EX(cache_slot + 2);
				if (!prop_info) {
					break;
				}
			}
			if (UNEXPECTED(!zend_handle_fetch_obj_flags(result, retval, zobj, prop_info, flags))) {
				return;
			}
		}
	} while (0);

	if (UNEXPECTED(Z_TYPE_P(retval) == IS_UNDEF)) {
		ZVAL_NULL(retval);
	}
}

static void ZEND_FASTCALL zend_jit_check_array_promotion(zval *val, zend_property_info *prop)
{
	zend_execute_data *execute_data = EG(current_execute_data);
	const zend_op *opline = execute_data->opline;
	zval *result = EX_VAR(opline->result.var);

	if (((Z_TYPE_P(val) <= IS_FALSE
	  || (Z_ISREF_P(val) && Z_TYPE_P(Z_REFVAL_P(val)) <= IS_FALSE))
	 &&	ZEND_TYPE_IS_SET(prop->type)
	 && ZEND_TYPE_FULL_MASK(prop->type) & (MAY_BE_ITERABLE|MAY_BE_ARRAY)) == 0) {
		zend_string *type_str = zend_type_to_string(prop->type);
		zend_type_error(
			"Cannot auto-initialize an array inside property %s::$%s of type %s",
			ZSTR_VAL(prop->ce->name), zend_get_unmangled_property_name(prop->name),
			ZSTR_VAL(type_str)
		);
		zend_string_release(type_str);
		ZVAL_ERROR(result);
	} else {
		ZVAL_INDIRECT(result, val);
	}
}

static void ZEND_FASTCALL zend_jit_create_typed_ref(zval *val, zend_property_info *prop, zval *result)
{
	if (!Z_ISREF_P(val)) {
		ZVAL_NEW_REF(val, val);
		ZEND_REF_ADD_TYPE_SOURCE(Z_REF_P(val), prop);
	}
	ZVAL_INDIRECT(result, val);
}

static void ZEND_FASTCALL zend_jit_extract_helper(zend_refcounted *garbage)
{
	zend_execute_data *execute_data = EG(current_execute_data);
	const zend_op *opline = execute_data->opline;
	zval *zv = EX_VAR(opline->result.var);

	if (EXPECTED(Z_TYPE_P(zv) == IS_INDIRECT)) {
		ZVAL_COPY(zv, Z_INDIRECT_P(zv));
	}
	rc_dtor_func(garbage);
}

static void ZEND_FASTCALL zend_jit_vm_stack_free_args_helper(zend_execute_data *call)
{
	zend_vm_stack_free_args(call);
}

static zend_always_inline void zend_jit_assign_to_typed_ref(zend_reference *ref, zval *value, zend_uchar value_type)
{
	zval variable;

	ZVAL_REF(&variable, ref);
	zend_assign_to_variable(&variable, value, value_type, ZEND_CALL_USES_STRICT_TYPES(EG(current_execute_data)));
}

static void ZEND_FASTCALL zend_jit_assign_const_to_typed_ref(zend_reference *ref, zval *value)
{
	zend_jit_assign_to_typed_ref(ref, value, IS_CONST);
}

static void ZEND_FASTCALL zend_jit_assign_tmp_to_typed_ref(zend_reference *ref, zval *value)
{
	zend_jit_assign_to_typed_ref(ref, value, IS_TMP_VAR);
}

static void ZEND_FASTCALL zend_jit_assign_var_to_typed_ref(zend_reference *ref, zval *value)
{
	zend_jit_assign_to_typed_ref(ref, value, IS_VAR);
}

static void ZEND_FASTCALL zend_jit_assign_cv_to_typed_ref(zend_reference *ref, zval *value)
{
	zend_jit_assign_to_typed_ref(ref, value, IS_CV);
}


static zend_property_info *zend_jit_get_prop_not_accepting_double(zend_reference *ref)
{
	zend_property_info *prop;
	ZEND_REF_FOREACH_TYPE_SOURCES(ref, prop) {
		if (!(ZEND_TYPE_FULL_MASK(prop->type) & MAY_BE_DOUBLE)) {
			return prop;
		}
	} ZEND_REF_FOREACH_TYPE_SOURCES_END();
	return NULL;
}

static ZEND_COLD void zend_jit_throw_incdec_ref_error(zend_reference *ref, zend_bool inc)
{
	zend_property_info *error_prop = zend_jit_get_prop_not_accepting_double(ref);
	/* Currently there should be no way for a typed reference to accept both int and double.
	 * Generalize this and the related property code once this becomes possible. */
	ZEND_ASSERT(error_prop);
	zend_type_error(
		"Cannot %s a reference held by property %s::$%s of type %sint past its %simal value",
		inc ? "increment" : "decrement",
		ZSTR_VAL(error_prop->ce->name),
		zend_get_unmangled_property_name(error_prop->name),
		ZEND_TYPE_ALLOW_NULL(error_prop->type) ? "?" : "",
		inc ? "max" : "min");
}

static void ZEND_FASTCALL zend_jit_pre_inc_typed_ref(zval *var_ptr, zend_reference *ref, zval *ret)
{
	zval tmp;

	ZVAL_COPY(&tmp, var_ptr);

	increment_function(var_ptr);

	if (UNEXPECTED(Z_TYPE_P(var_ptr) == IS_DOUBLE) && Z_TYPE(tmp) == IS_LONG) {
		zend_jit_throw_incdec_ref_error(ref, 1);
		ZVAL_COPY_VALUE(var_ptr, &tmp);
	} else if (UNEXPECTED(!zend_verify_ref_assignable_zval(ref, var_ptr, ZEND_CALL_USES_STRICT_TYPES(EG(current_execute_data))))) {
		zval_ptr_dtor(var_ptr);
		ZVAL_COPY_VALUE(var_ptr, &tmp);
	} else {
		zval_ptr_dtor(&tmp);
	}
	if (ret) {
		ZVAL_COPY(ret, var_ptr);
	}
}

static void ZEND_FASTCALL zend_jit_pre_dec_typed_ref(zval *var_ptr, zend_reference *ref, zval *ret)
{
	zval tmp;

	ZVAL_COPY(&tmp, var_ptr);

	decrement_function(var_ptr);

	if (UNEXPECTED(Z_TYPE_P(var_ptr) == IS_DOUBLE) && Z_TYPE(tmp) == IS_LONG) {
		zend_jit_throw_incdec_ref_error(ref, 0);
		ZVAL_COPY_VALUE(var_ptr, &tmp);
	} else if (UNEXPECTED(!zend_verify_ref_assignable_zval(ref, var_ptr, ZEND_CALL_USES_STRICT_TYPES(EG(current_execute_data))))) {
		zval_ptr_dtor(var_ptr);
		ZVAL_COPY_VALUE(var_ptr, &tmp);
	} else {
		zval_ptr_dtor(&tmp);
	}
	if (ret) {
		ZVAL_COPY(ret, var_ptr);
	}
}

static void ZEND_FASTCALL zend_jit_post_inc_typed_ref(zval *var_ptr, zend_reference *ref, zval *ret)
{
	ZVAL_COPY(ret, var_ptr);

	increment_function(var_ptr);

	if (UNEXPECTED(Z_TYPE_P(var_ptr) == IS_DOUBLE) && Z_TYPE_P(ret) == IS_LONG) {
		zend_jit_throw_incdec_ref_error(ref, 1);
		ZVAL_COPY_VALUE(var_ptr, ret);
	} else if (UNEXPECTED(!zend_verify_ref_assignable_zval(ref, var_ptr, ZEND_CALL_USES_STRICT_TYPES(EG(current_execute_data))))) {
		zval_ptr_dtor(var_ptr);
		ZVAL_COPY_VALUE(var_ptr, ret);
	}
}

static void ZEND_FASTCALL zend_jit_post_dec_typed_ref(zval *var_ptr, zend_reference *ref, zval *ret)
{
	ZVAL_COPY(ret, var_ptr);

	decrement_function(var_ptr);

	if (UNEXPECTED(Z_TYPE_P(var_ptr) == IS_DOUBLE) && Z_TYPE_P(ret) == IS_LONG) {
		zend_jit_throw_incdec_ref_error(ref, 0);
		ZVAL_COPY_VALUE(var_ptr, ret);
	} else if (UNEXPECTED(!zend_verify_ref_assignable_zval(ref, var_ptr, ZEND_CALL_USES_STRICT_TYPES(EG(current_execute_data))))) {
		zval_ptr_dtor(var_ptr);
		ZVAL_COPY_VALUE(var_ptr, ret);
	}
}

static void ZEND_FASTCALL zend_jit_assign_op_to_typed_ref(zend_reference *ref, zval *val, binary_op_type binary_op)
{
	zval z_copy;

	binary_op(&z_copy, &ref->val, val);
	if (EXPECTED(zend_verify_ref_assignable_zval(ref, &z_copy, ZEND_CALL_USES_STRICT_TYPES(EG(current_execute_data))))) {
		zval_ptr_dtor(&ref->val);
		ZVAL_COPY_VALUE(&ref->val, &z_copy);
	} else {
		zval_ptr_dtor(&z_copy);
	}
}

static void ZEND_FASTCALL zend_jit_only_vars_by_reference(zval *arg)
{
	ZVAL_NEW_REF(arg, arg);
	zend_error(E_NOTICE, "Only variables should be passed by reference");
}

static void ZEND_FASTCALL zend_jit_invalid_array_access(zval *container)
{
	zend_error(E_WARNING, "Trying to access array offset on value of type %s", zend_zval_type_name(container));
}

static void ZEND_FASTCALL zend_jit_invalid_property_read(zval *container, const char *property_name)
{
	zend_error(E_WARNING, "Attempt to read property '%s' on %s", property_name, zend_zval_type_name(container));
}

static void ZEND_FASTCALL zend_jit_invalid_property_write(zval *container, const char *property_name)
{
	zend_throw_error(NULL,
		"Attempt to modify property '%s' on %s",
		property_name, zend_zval_type_name(container));
}

static zval * ZEND_FASTCALL zend_jit_prepare_assign_dim_ref(zval *ref) {
	zval *val = Z_REFVAL_P(ref);
	if (Z_TYPE_P(val) <= IS_FALSE) {
		if (ZEND_REF_HAS_TYPE_SOURCES(Z_REF_P(ref))
				&& !zend_verify_ref_array_assignable(Z_REF_P(ref))) {
			return NULL;
		}
		ZVAL_ARR(val, zend_new_array(8));
	}
	return val;
}

static void ZEND_FASTCALL zend_jit_pre_inc(zval *var_ptr, zval *ret)
{
	increment_function(var_ptr);
	ZVAL_COPY(ret, var_ptr);
}

static void ZEND_FASTCALL zend_jit_pre_dec(zval *var_ptr, zval *ret)
{
	decrement_function(var_ptr);
	ZVAL_COPY(ret, var_ptr);
}

#define HT_POISONED_PTR ((HashTable *) (intptr_t) -1)

static zend_never_inline void ZEND_FASTCALL _zend_hash_iterators_remove(HashTable *ht)
{
	HashTableIterator *iter = EG(ht_iterators);
	HashTableIterator *end  = iter + EG(ht_iterators_used);

	while (iter != end) {
		if (iter->ht == ht) {
			iter->ht = HT_POISONED_PTR;
		}
		iter++;
	}
}

static void ZEND_FASTCALL zend_jit_array_free(HashTable *ht)
{
	GC_REMOVE_FROM_BUFFER(ht);
	if (UNEXPECTED(HT_HAS_ITERATORS(ht))) {
		_zend_hash_iterators_remove(ht);
	}
	if (!(EXPECTED(HT_FLAGS(ht) & HASH_FLAG_UNINITIALIZED))) {
		efree(HT_GET_DATA_ADDR(ht));
	}
	FREE_HASHTABLE(ht);
}

static HashTable *ZEND_FASTCALL zend_jit_zval_array_dup(zval *arr)
{
	HashTable *ht;

	Z_TRY_DELREF_P(arr);
	ht = Z_ARRVAL_P(arr);
	ht = zend_array_dup(ht);
	ZVAL_ARR(arr, ht);
	return ht;
}
