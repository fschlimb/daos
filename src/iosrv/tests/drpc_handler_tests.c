/*
 * (C) Copyright 2019 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. 8F-30005.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

/*
 * Unit tests for the drpc handler registration system
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <daos/drpc.pb-c.h>
#include <daos/drpc.h>
#include <daos/test_mocks.h>
#include <daos/test_utils.h>
#include "../drpc_handler.h"

/*
 * Some dummy handlers so we have different ptrs for each test registration
 */
static void
dummy_drpc_handler1(Drpc__Call *request, Drpc__Response **response)
{
}

static void
dummy_drpc_handler2(Drpc__Call *request, Drpc__Response **response)
{
}

static drpc_handler_t handler_funcs[] = {
		dummy_drpc_handler1,
		dummy_drpc_handler2
};

/*
 * Helper functions used by unit tests
 */

static struct dss_drpc_handler *
create_handler_list(int num_items)
{
	struct dss_drpc_handler	*list;
	int			i;

	D_ASSERT(num_items <= (sizeof(handler_funcs) / sizeof(drpc_handler_t)));

	D_ALLOC_ARRAY(list, num_items + 1);

	for (i = 0; i < num_items; i++) {
		list[i].module_id = i;
		list[i].handler = handler_funcs[i];
	}

	return list;
}

static void
destroy_handler_list(struct dss_drpc_handler *list)
{
	D_FREE(list);
}

/*
 * Test setup and teardown
 * Initializes and destroys the registry by default.
 */
static int
drpc_hdlr_test_setup(void **state)
{
	return drpc_hdlr_init();
}

static int
drpc_hdlr_test_teardown(void **state)
{
	return drpc_hdlr_fini();
}

/*
 * Registration unit tests
 */
static void
drpc_hdlr_register_with_null_handler(void **state)
{
	assert_int_equal(drpc_hdlr_register(0, NULL), -DER_INVAL);
}

static void
drpc_hdlr_register_with_good_handler(void **state)
{
	assert_int_equal(drpc_hdlr_register(DRPC_MODULE_TEST,
			dummy_drpc_handler1), DER_SUCCESS);

	assert_ptr_equal(drpc_hdlr_get_handler(DRPC_MODULE_TEST),
			dummy_drpc_handler1);
}

static void
drpc_hdlr_register_same_id_twice(void **state)
{
	assert_int_equal(drpc_hdlr_register(DRPC_MODULE_TEST,
			dummy_drpc_handler1), DER_SUCCESS);
	assert_int_equal(drpc_hdlr_register(DRPC_MODULE_TEST,
			dummy_drpc_handler2), -DER_EXIST);

	/* Should be unchanged */
	assert_ptr_equal(drpc_hdlr_get_handler(0), dummy_drpc_handler1);
}

static void
drpc_hdlr_register_null_handler_after_good_one(void **state)
{
	assert_int_equal(drpc_hdlr_register(DRPC_MODULE_TEST,
			dummy_drpc_handler1), DER_SUCCESS);
	assert_int_equal(drpc_hdlr_register(DRPC_MODULE_TEST, NULL),
			-DER_INVAL);

	/* Should be unchanged */
	assert_ptr_equal(drpc_hdlr_get_handler(DRPC_MODULE_TEST),
			dummy_drpc_handler1);
}

static void
drpc_hdlr_register_bad_module_id(void **state)
{
	assert_int_equal(drpc_hdlr_register(NUM_DRPC_MODULES,
			dummy_drpc_handler2), -DER_INVAL);
}

static void
drpc_hdlr_get_handler_with_unregistered_id(void **state)
{
	drpc_hdlr_register(DRPC_MODULE_TEST, dummy_drpc_handler1);

	assert_ptr_equal(drpc_hdlr_get_handler(DRPC_MODULE_TEST + 1),
			NULL);
}

static void
drpc_hdlr_get_handler_with_invalid_id(void **state)
{
	assert_ptr_equal(drpc_hdlr_get_handler(NUM_DRPC_MODULES),
			NULL);
}

static void
drpc_hdlr_register_multiple(void **state)
{
	assert_int_equal(drpc_hdlr_register(DRPC_MODULE_TEST,
			dummy_drpc_handler1), DER_SUCCESS);
	assert_int_equal(drpc_hdlr_register(DRPC_MODULE_SECURITY_AGENT,
			dummy_drpc_handler2), DER_SUCCESS);

	assert_ptr_equal(drpc_hdlr_get_handler(DRPC_MODULE_TEST),
			dummy_drpc_handler1);
	assert_ptr_equal(drpc_hdlr_get_handler(DRPC_MODULE_SECURITY_AGENT),
			dummy_drpc_handler2);
}

static void
drpc_hdlr_unregister_id_not_found(void **state)
{
	drpc_hdlr_register(DRPC_MODULE_TEST, dummy_drpc_handler1);

	/*
	 * It is already unregistered - We did nothing but the caller is
	 * satisfied.
	 */
	assert_int_equal(drpc_hdlr_unregister(DRPC_MODULE_SECURITY_AGENT),
			DER_SUCCESS);

	/* Ensure nothing was deleted */
	assert_non_null(drpc_hdlr_get_handler(DRPC_MODULE_TEST));
}

static void
drpc_hdlr_unregister_bad_module_id(void **state)
{
	assert_int_equal(drpc_hdlr_unregister(NUM_DRPC_MODULES),
			-DER_INVAL);
}

static void
drpc_hdlr_unregister_success(void **state)
{
	drpc_hdlr_register(DRPC_MODULE_TEST, dummy_drpc_handler1);
	drpc_hdlr_register(DRPC_MODULE_SECURITY_AGENT, dummy_drpc_handler2);

	assert_int_equal(drpc_hdlr_unregister(DRPC_MODULE_TEST),
			DER_SUCCESS);

	/* Ensure only the correct item was deleted */
	assert_null(drpc_hdlr_get_handler(DRPC_MODULE_TEST));
	assert_non_null(drpc_hdlr_get_handler(DRPC_MODULE_SECURITY_AGENT));
}

static void
drpc_hdlr_register_all_with_null(void **state)
{
	assert_int_equal(drpc_hdlr_register_all(NULL), DER_SUCCESS);
}

static void
drpc_hdlr_register_all_with_empty_list(void **state)
{
	struct dss_drpc_handler *empty = create_handler_list(0);

	assert_int_equal(drpc_hdlr_register_all(empty), DER_SUCCESS);

	destroy_handler_list(empty);
}

static void
drpc_hdlr_register_all_with_one_item(void **state)
{
	struct dss_drpc_handler *handlers = create_handler_list(1);

	assert_int_equal(drpc_hdlr_register_all(handlers), DER_SUCCESS);

	assert_ptr_equal(drpc_hdlr_get_handler(DRPC_MODULE_TEST),
			handlers[DRPC_MODULE_TEST].handler);

	destroy_handler_list(handlers);
}

static void
drpc_hdlr_register_all_with_multiple_items(void **state)
{
	int			num_items = NUM_DRPC_MODULES;
	int			i;
	struct dss_drpc_handler	*handlers = create_handler_list(num_items);

	assert_int_equal(drpc_hdlr_register_all(handlers), DER_SUCCESS);

	for (i = 0; i < num_items; i++) {
		assert_ptr_equal(drpc_hdlr_get_handler(i),
				handlers[i].handler);
	}

	destroy_handler_list(handlers);
}

static void
drpc_hdlr_register_all_with_duplicate(void **state)
{
	int			num_items = NUM_DRPC_MODULES;
	int			dup_idx = num_items - 1;
	int			i;
	struct dss_drpc_handler	*dup_list = create_handler_list(num_items);

	/* Make one of them a duplicate module ID */
	dup_list[dup_idx].module_id = DRPC_MODULE_TEST;

	assert_int_equal(drpc_hdlr_register_all(dup_list), -DER_EXIST);

	/* Should have registered all the ones we could */
	for (i = 0; i < num_items; i++) {
		if (i != dup_idx) { /* dup is the one that fails */
			assert_ptr_equal(drpc_hdlr_get_handler(i),
					dup_list[i].handler);
		}
	}

	destroy_handler_list(dup_list);
}

static void
drpc_hdlr_unregister_all_with_null(void **state)
{
	assert_int_equal(drpc_hdlr_unregister_all(NULL), DER_SUCCESS);
}

static void
drpc_hdlr_unregister_all_with_empty_list(void **state)
{
	struct dss_drpc_handler *empty = create_handler_list(0);

	assert_int_equal(drpc_hdlr_unregister_all(empty), DER_SUCCESS);

	destroy_handler_list(empty);
}

static void
drpc_hdlr_unregister_all_with_one_item(void **state)
{
	struct dss_drpc_handler *handlers = create_handler_list(1);

	/* Register them first */
	drpc_hdlr_register_all(handlers);

	assert_int_equal(drpc_hdlr_unregister_all(handlers), DER_SUCCESS);

	/* Make sure it was unregistered */
	assert_null(drpc_hdlr_get_handler(handlers[0].module_id));

	destroy_handler_list(handlers);
}

static void
drpc_hdlr_unregister_all_with_multiple_items(void **state)
{
	int			num_items = NUM_DRPC_MODULES;
	int			i;
	struct dss_drpc_handler	*handlers = create_handler_list(num_items);

	/* Register them first */
	drpc_hdlr_register_all(handlers);

	assert_int_equal(drpc_hdlr_unregister_all(handlers), DER_SUCCESS);

	/* Make sure they were all unregistered */
	for (i = 0; i < num_items; i++) {
		assert_null(drpc_hdlr_get_handler(handlers[i].module_id));
	}

	destroy_handler_list(handlers);
}

/*
 * Tests for when the registry table is uninitialized.
 * Don't use the standard setup/teardown functions with these.
 */
static void
drpc_hdlr_register_uninitialized(void **state)
{
	assert_int_equal(drpc_hdlr_register(0, dummy_drpc_handler1),
			-DER_UNINIT);
}

static void
drpc_hdlr_get_handler_uninitialized(void **state)
{
	assert_ptr_equal(drpc_hdlr_get_handler(0), NULL);
}

static void
drpc_hdlr_unregister_uninitialized(void **state)
{
	assert_int_equal(drpc_hdlr_unregister(0), -DER_UNINIT);
}

static void
drpc_hdlr_register_all_uninitialized(void **state)
{
	struct dss_drpc_handler *list = create_handler_list(0);

	assert_int_equal(drpc_hdlr_register_all(list), -DER_UNINIT);

	destroy_handler_list(list);
}

static void
drpc_hdlr_unregister_all_uninitialized(void **state)
{
	struct dss_drpc_handler *list = create_handler_list(0);

	assert_int_equal(drpc_hdlr_unregister_all(list), -DER_UNINIT);

	destroy_handler_list(list);
}

/* Convenience macros for unit tests */
#define UTEST(x)	cmocka_unit_test_setup_teardown(x,	\
				drpc_hdlr_test_setup,		\
				drpc_hdlr_test_teardown)
#define UTEST_NO_INIT(x)	cmocka_unit_test(x)

int
main(void)
{
	const struct CMUnitTest tests[] = {
		UTEST(drpc_hdlr_register_with_null_handler),
		UTEST(drpc_hdlr_register_with_good_handler),
		UTEST(drpc_hdlr_register_same_id_twice),
		UTEST(drpc_hdlr_register_null_handler_after_good_one),
		UTEST(drpc_hdlr_register_bad_module_id),
		UTEST(drpc_hdlr_get_handler_with_unregistered_id),
		UTEST(drpc_hdlr_get_handler_with_invalid_id),
		UTEST(drpc_hdlr_register_multiple),
		UTEST(drpc_hdlr_unregister_id_not_found),
		UTEST(drpc_hdlr_unregister_bad_module_id),
		UTEST(drpc_hdlr_unregister_success),
		UTEST(drpc_hdlr_register_all_with_null),
		UTEST(drpc_hdlr_register_all_with_empty_list),
		UTEST(drpc_hdlr_register_all_with_one_item),
		UTEST(drpc_hdlr_register_all_with_multiple_items),
		UTEST(drpc_hdlr_register_all_with_duplicate),
		UTEST(drpc_hdlr_unregister_all_with_null),
		UTEST(drpc_hdlr_unregister_all_with_empty_list),
		UTEST(drpc_hdlr_unregister_all_with_one_item),
		UTEST(drpc_hdlr_unregister_all_with_multiple_items),

		/* Uninitialized cases */
		UTEST_NO_INIT(drpc_hdlr_register_uninitialized),
		UTEST_NO_INIT(drpc_hdlr_get_handler_uninitialized),
		UTEST_NO_INIT(drpc_hdlr_unregister_uninitialized),
		UTEST_NO_INIT(drpc_hdlr_register_all_uninitialized),
		UTEST_NO_INIT(drpc_hdlr_unregister_all_uninitialized)
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}

#undef UTEST_NO_INIT
#undef UTEST