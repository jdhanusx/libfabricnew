#include "efa_unit_tests.h"

/*
 * Only works on nodes with EFA devices
 * This test calls fi_av_insert() twice with the same raw address,
 * and verifies that returned fi_addr is the same and
 * ibv_create_ah only gets called once.
 */
void test_av_insert_duplicate_raw_addr()
{
	struct efa_resource resource = {0};
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(struct efa_ep_addr);
	fi_addr_t addr1, addr2;
	int err, num_addr;
	int ibv_create_ah_call_counter_before_insert;

	err = efa_unit_test_resource_construct(&resource, FI_EP_RDM);
	assert_int_equal(err, 0);
	g_efa_unit_test_mocks.ibv_create_ah = &efa_mock_ibv_create_ah_increase_call_counter;

	err = fi_getname(&resource.ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(err, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;

	ibv_create_ah_call_counter_before_insert = g_ibv_create_ah_call_counter;
	num_addr = fi_av_insert(resource.av, &raw_addr, 1, &addr1, 0 /* flags */, NULL /* context */);
	assert_int_equal(num_addr, 1);
	assert_int_equal(ibv_create_ah_call_counter_before_insert + 1, g_ibv_create_ah_call_counter);

	ibv_create_ah_call_counter_before_insert = g_ibv_create_ah_call_counter;
	num_addr = fi_av_insert(resource.av, &raw_addr, 1, &addr2, 0 /* flags */, NULL /* context */);
	assert_int_equal(num_addr, 1);
	assert_int_equal(ibv_create_ah_call_counter_before_insert, g_ibv_create_ah_call_counter);
	assert_int_equal(addr1, addr2);

	g_efa_unit_test_mocks.ibv_create_ah = __real_ibv_create_ah;
	efa_unit_test_resource_destruct(&resource);
}

/*
 * Only works on nodes with EFA devices
 * This test calls fi_av_insert() twice with two difference raw address with same GID,
 * and verifies that returned fi_addr is different and ibv_create_ah only gets called once.
 * this is because libfabric EFA provider has a cache for address handle (AH).
 */
void test_av_insert_duplicate_gid()
{
	struct efa_resource resource = {0};
	struct efa_ep_addr raw_addr = {0};
	size_t raw_addr_len = sizeof(struct efa_ep_addr);
	fi_addr_t addr1, addr2;
	int err, num_addr;
	int ibv_create_ah_call_counter_before_insert;

	err = efa_unit_test_resource_construct(&resource, FI_EP_RDM);
	assert_int_equal(err, 0);
	g_efa_unit_test_mocks.ibv_create_ah = &efa_mock_ibv_create_ah_increase_call_counter;

	err = fi_getname(&resource.ep->fid, &raw_addr, &raw_addr_len);
	assert_int_equal(err, 0);
	raw_addr.qpn = 1;
	raw_addr.qkey = 0x1234;

	ibv_create_ah_call_counter_before_insert = g_ibv_create_ah_call_counter;
	num_addr = fi_av_insert(resource.av, &raw_addr, 1, &addr1, 0 /* flags */, NULL /* context */);
	assert_int_equal(num_addr, 1);
	assert_int_equal(ibv_create_ah_call_counter_before_insert + 1, g_ibv_create_ah_call_counter);

	raw_addr.qpn = 2;
	raw_addr.qkey = 0x5678;
	ibv_create_ah_call_counter_before_insert = g_ibv_create_ah_call_counter;
	num_addr = fi_av_insert(resource.av, &raw_addr, 1, &addr2, 0 /* flags */, NULL /* context */);
	assert_int_equal(num_addr, 1);
	assert_int_equal(ibv_create_ah_call_counter_before_insert, g_ibv_create_ah_call_counter);
	assert_int_not_equal(addr1, addr2);

	g_efa_unit_test_mocks.ibv_create_ah = __real_ibv_create_ah;
	efa_unit_test_resource_destruct(&resource);
}

