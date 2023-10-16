---
layout: page
title: fi_av(3)
tagline: Libfabric Programmer's Manual
---
{% include JB/setup %}

# NAME

fi_av \- Address vector operations

fi_av_open / fi_close
: Open or close an address vector

fi_av_insert / fi_av_insertsvc / fi_av_remove
: Insert/remove an address into/from the address vector.

fi_av_lookup
: Retrieve an address stored in the address vector.

fi_av_straddr
: Convert an address into a printable string.

# SYNOPSIS

```c
#include <rdma/fi_domain.h>

int fi_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
    struct fid_av **av, void *context);

int fi_close(struct fid *av);

int fi_av_insert(struct fid_av *av, void *addr, size_t count,
    fi_addr_t *fi_addr, uint64_t flags, void *context);

int fi_av_insertsvc(struct fid_av *av, const char *node,
    const char *service, fi_addr_t *fi_addr, uint64_t flags,
    void *context);

int fi_av_insertsym(struct fid_av *av, const char *node,
    size_t nodecnt, const char *service, size_t svccnt,
    fi_addr_t *fi_addr, uint64_t flags, void *context);

int fi_av_remove(struct fid_av *av, fi_addr_t *fi_addr, size_t count,
    uint64_t flags);

int fi_av_lookup(struct fid_av *av, fi_addr_t fi_addr,
    void *addr, size_t *addrlen);

fi_addr_t fi_rx_addr(fi_addr_t fi_addr, int rx_index,
	  int rx_ctx_bits);

fi_addr_t fi_group_addr(fi_addr_t fi_addr, uint32_t group_id);

const char * fi_av_straddr(struct fid_av *av, const void *addr,
      char *buf, size_t *len);
```

# ARGUMENTS

*domain*
: Resource domain

*av*
: Address vector

*eq*
: Event queue

*attr*
: Address vector attributes

*context*
: User specified context associated with the address vector or insert
  operation.

*addr*
: Buffer containing one or more addresses to insert into address vector.

*addrlen*
: On input, specifies size of addr buffer.  On output, stores number
  of bytes written to addr buffer.

*fi_addr*
: For insert, a reference to an array where returned fabric addresses
  will be written.  For remove, one or more fabric addresses to remove.
  If FI_AV_USER_ID is requested, also used as input into insert calls
  to assign the user ID with the added address.

*count*
: Number of addresses to insert/remove from an AV.

*flags*
: Additional flags to apply to the operation.

# DESCRIPTION

Address vectors are used to map higher-level addresses, which may be
more natural for an application to use, into fabric specific
addresses.  For example, an endpoint may be associated with a
struct sockaddr_in address, indicating the endpoint is reachable using
a TCP port number over an IPv4 address.  This may hold even if the
endpoint communicates using a proprietary network protocol.  The
purpose of the AV is to associate a higher-level address with
a simpler, more efficient value that can be used by the libfabric API in a
fabric agnostic way.  The mapped address is of type fi_addr_t and is
returned through an AV insertion call.

The process of mapping an address is fabric and provider specific,
but may involve lengthy address resolution and fabric management
protocols.  AV operations are synchronous by default.  See
the NOTES section for AV restrictions on duplicate addresses.

## fi_av_open

fi_av_open allocates or opens an address vector.  The properties and
behavior of the address vector are defined by `struct fi_av_attr`.

```c
struct fi_av_attr {
	enum fi_av_type  type;        /* type of AV */
	int              rx_ctx_bits; /* address bits to identify rx ctx */
	size_t           count;       /* # entries for AV */
	size_t           ep_per_node; /* # endpoints per fabric address */
	const char       *name;       /* system name of AV */
	void             *map_addr;   /* base mmap address */
	uint64_t         flags;       /* operation flags */
};
```

*type*
: This field provides compatibility with the libfabric version 1 series.
  The AV type defines a conceptual implementation of an address
  vector as visible to the application.  The type specifies how an
  application views data stored in the AV along with requirements on
  how addresses are accessed.  Valid values are:

- *FI_AV_TABLE*
: Addresses inserted into an AV of type FI_AV_TABLE are accessible using
  a simple index.  Conceptually, the AV may be treated as an array of
  addresses.  When FI_AV_TABLE is used, the assigned fi_addr_t to an inserted
  address is index that corresponds to its insertion order into the table.
  The index of the first address inserted into an FI_AV_TABLE will be 0,
  and successive insertions will be given sequential indices.  Sequential
  indices will be assigned across insertion calls on the same AV.  Because
  the fi_addr_t values returned from an insertion call are deterministic,
  applications may not need to provide the fi_addr_t output parameters to
  insertion calls.  The exception is when authentication keys are required
  for communication.

  By default, all AVs act as FI_AV_TABLE.

- *FI_AV_MAP*
: In the libfabric version 1 series, FI_AV_MAP allowed the provider to assign
  an arbitrary value (such as a virtual address) to the fi_addr_t value
  associated with an inserted address.  As a result, the use of FI_AV_MAP required
  that an application store the returned fi_addr_t value associated with each
  inserted address.  In the version 2 series, the behavior of FI_AV_MAP is
  aligned with that of FI_AV_TABLE.  The returned fi_addr_t values will
  correspond with an index based on the address' insertion order.  An exception
  is made when authentication keys are required for communication.

- *FI_AV_UNSPEC*
: Provider will choose its preferred AV type. The AV type used will
  be returned through the type field in fi_av_attr.

*Receive Context Bits (rx_ctx_bits)*
: The receive context bits field is only for use with scalable
  endpoints.  It indicates the number of bits reserved in a returned
  fi_addr_t, which will be used to identify a specific target receive
  context.  See fi_rx_addr() and fi_endpoint(3) for additional details
  on receive contexts.  The requested number of bits should be
  selected such that 2 ^ rx_ctx_bits >= rx_ctx_cnt for the endpoint.

*count*
: Indicates the expected number of addresses that will be inserted
  into the AV.  The provider uses this to optimize resource
  allocations.

*ep_per_node*
: This field indicates the number of endpoints that will be associated
  with a specific fabric, or network, address.  If the number of
  endpoints per node is unknown, this value should be set to 0.  The
  provider uses this value to optimize resource allocations.  For
  example, distributed, parallel applications may set this to the
  number of processes allocated per node, times the number of
  endpoints each process will open.

*name*
: An optional system name associated with the address vector to create
  or open.  Address vectors may be shared across multiple processes
  which access the same named domain on the same node.  The name field
  allows the underlying provider to identify a shared AV.

  If the name field is non-NULL and the AV is not opened for read-only
  access, a named AV will be created, if it does not already exist.

*map_addr*
: The map_addr determines the base fi_addr_t address that a provider
  should use when sharing an AV of type FI_AV_MAP between processes.
  Processes that provide the same value for map_addr to a shared AV
  may use the same fi_addr_t values returned from an fi_av_insert call.

  The map_addr may be used by the provider to mmap memory allocated
  for a shared AV between processes; however, the provider is not
  required to use the map_addr in this fashion.  The only requirement
  is that an fi_addr_t returned as part of an fi_av_insert call on one
  process is usable on another process which opens an AV of the same
  name at the same map_addr value.  The relationship between the
  map_addr and any returned fi_addr_t is not defined.

  If name is non-NULL and map_addr is 0, then the map_addr used by the
  provider will be returned through the attribute structure.  The
  map_addr field is ignored if name is NULL.

*flags*
: The following flags may be used when opening an AV.

- *FI_READ*
: Opens an AV for read-only access.  An AV opened for read-only access
  must be named (name attribute specified), and the AV must exist.

- *FI_SYMMETRIC*
: Indicates that each node will be associated with the same number of
  endpoints, the same transport addresses will be allocated on each
  node, and the transport addresses will be sequential.  This feature
  targets distributed applications on large fabrics and allows for
  highly-optimized storage of remote endpoint addressing.

## fi_close

The fi_close call is used to release all resources associated with an
address vector.  Note that any events queued on an event queue referencing
the AV are left untouched.  It is recommended that callers retrieve all
events associated with the AV before closing it.

When closing the address vector, there must be no opened endpoints associated
with the AV.  If resources are still associated with the AV when attempting to
close, the call will return -FI_EBUSY.

## fi_av_insert

The fi_av_insert call inserts zero or more addresses into an AV.  The
number of addresses is specified through the count parameter.  The
addr parameter references an array of addresses to insert into the AV.
Addresses inserted into an address vector must be in the same format
as specified in the addr_format field of the fi_info struct provided when
opening the corresponding domain. When using the `FI_ADDR_STR` format,
the `addr` parameter should reference an array of strings (char \*\*).

Inserted addresses are placed into the table in order.  An address is
inserted at the lowest index that corresponds to
an unused table location, with indices starting at 0.  That is, the
first address inserted may be referenced at index 0, the second at index
1, and so forth.  When addresses are inserted into an AV table,
the assigned fi_addr values will be simple indices
corresponding to the entry into the table where the address was
inserted.  Index values accumulate across successive insert calls
in the order the calls are made, not necessarily in the order the
insertions complete.

Because insertions occur at a pre-determined index, the fi_addr
parameter may be NULL.  If fi_addr is non-NULL, it must reference
an array of fi_addr_t, and the buffer must remain valid until the
insertion operation completes.  Note that if fi_addr is NULL and
the FI_SYNC_ERR flag is not set, individual
insertion failures cannot be reported and the application must use
other calls, such as `fi_av_lookup` to learn which specific addresses
failed to insert.

*flags*
: The following flag may be passed to AV insertion calls: fi_av_insert,
  fi_av_insertsvc, or fi_av_insertsym.

- *FI_MORE*
: In order to allow optimized address insertion, the application may
  specify the FI_MORE flag to the insert call to give a hint to the
  provider that more insertion requests will follow, allowing the
  provider to aggregate insertion requests if desired.  An application
  may make any number of insertion calls with FI_MORE set, provided
  that they are followed by an insertion call without FI_MORE.  This
  signifies to the provider that the insertion list is complete.
  Providers are free to ignore FI_MORE.

- *FI_SYNC_ERR*
: This flag may be used to retrieve error details of failed insertions.
  If set, the context parameter of insertion calls references an array
  of integers, with context set to address of the first element of the array.
  The resulting status of attempting to insert each address will be
  written to the corresponding array location.  Successful insertions
  will be updated to 0.  Failures will contain a fabric errno code.

- *FI_AV_USER_ID*
: This flag associates a user-assigned identifier with each AV entry
  that is returned with any completion entry in place of the AV's address.
  See the user ID section below.

## fi_av_insertsvc

The fi_av_insertsvc call behaves similar to fi_av_insert, but allows the
application to specify the node and service names, similar to the
fi_getinfo inputs, rather than an encoded address.  The node and service
parameters are defined the same as fi_getinfo(3).  Node should be a string
that corresponds to a hostname or network address.  The service string
corresponds to a textual representation of a transport address.
Applications may also pass in an `FI_ADDR_STR` formatted address as the
node parameter. In such cases, the service parameter must be NULL. See
fi_getinfo.3 for details on using `FI_ADDR_STR`. Supported flags are the
same as for fi_av_insert.

## fi_av_insertsym

fi_av_insertsym performs a symmetric insert that inserts a sequential
range of nodes and/or service addresses into an AV.  The svccnt
parameter indicates the number of transport (endpoint) addresses to
insert into the AV for each node address, with the service parameter
specifying the starting transport address.  Inserted transport
addresses will be of the range {service, service + svccnt - 1},
inclusive.  All service addresses for a node will be inserted before
the next node is inserted.

The nodecnt parameter indicates the number of node (network) addresses
to insert into the AV, with the node parameter specifying the starting
node address.  Inserted node addresses will be of the range {node,
node + nodecnt - 1}, inclusive.  If node is a non-numeric string, such
as a hostname, it must contain a numeric suffix if nodecnt > 1.

As an example, if node = "10.1.1.1", nodecnt = 2, service = "5000",
and svccnt = 2, the following addresses will be inserted into the AV
in the order shown: 10.1.1.1:5000, 10.1.1.1:5001, 10.1.1.2:5000,
10.1.1.2:5001.  If node were replaced by the hostname "host10", the
addresses would be: host10:5000, host10:5001, host11:5000,
host11:5001.

The total number of inserted addresses will be nodecnt x svccnt.

Supported flags are the same as for fi_av_insert.

## fi_av_remove

fi_av_remove removes a set of addresses from an address vector.
The corresponding fi_addr_t values are invalidated and may not
be used in data transfer calls.  The behavior of operations in
progress that reference the removed addresses is undefined.

Note that removing an address may not disable receiving data from the
peer endpoint.  fi_av_close will automatically cleanup any associated
resources.

Flags are reserved for future use and must be 0.

## fi_av_lookup

This call returns the address stored in the address vector that
corresponds to the given fi_addr.  The returned address is the same
format as those stored by the AV.  On input, the addrlen parameter
should indicate the size of the addr buffer.  If the actual address is
larger than what can fit into the buffer, it will be truncated.  On
output, addrlen is set to the size of the buffer needed to store the
address, which may be larger than the input value.

## fi_rx_addr

This function is used to convert an endpoint address, returned by
fi_av_insert, into an address that specifies a target receive context.
The value for rx_ctx_bits must match that specified in
the AV attributes for the given address.

Connected endpoints that support multiple receive contexts, but are
not associated with address vectors should specify FI_ADDR_NOTAVAIL
for the fi_addr parameter.

## fi_av_straddr

The fi_av_straddr function converts the provided address into a
printable string.  The specified address must be of the same format as
those stored by the AV, though the address itself is not required to
have been inserted.  On input, the len parameter should specify the
size of the buffer referenced by buf.  On output, addrlen is set to the
size of the buffer needed to store the address.  This size may be
larger than the input len.  If the provided buffer is too small, the
results will be truncated.  fi_av_straddr returns a pointer to buf.

# NOTES

An AV should only store a single instance of an address.
Attempting to insert a duplicate copy of the same address into an AV
may result in undefined behavior, depending on the provider implementation.
Providers are not required to check for duplicates, as doing so could
incur significant overhead to the insertion process. For portability,
applications may need to track which peer addresses have been inserted
into a given AV in order to avoid duplicate entries.  However, providers are
required to support the removal, followed by the re-insertion of an
address.  Only duplicate insertions are restricted.

# USER IDENTIFIERS FOR ADDRESSES

As described above, endpoint addresses that are inserted into an AV are
mapped to an fi_addr_t value.  The fi_addr_t is used in data transfer APIs
to specify the destination of an outbound transfer, in receive APIs to
indicate the source for an inbound transfer, and also in completion events
to report the source address of inbound transfers.  The FI_AV_USER_ID
capability bit and flag provide a mechanism by which the fi_addr_t value
reported by a completion event is replaced with a user-specified value
instead.  This is useful for applications that need to map the source
address to their own data structure.

Support for FI_AV_USER_ID is provider specific, as it may not be feasible
for a provider to implement this support without significant overhead.
For example, some providers may need to add a reverse lookup mechanism.
This feature may be unavailable if shared AVs are requested, or
negatively impact the per process memory footprint if implemented.  For
providers that do not support FI_AV_USER_ID, users may be able to trade
off lookup processing with protocol overhead, by carrying source
identification within a message header.

User-specified fi_addr_t values are provided as part of address insertion
(e.g. fi_av_insert) through the fi_addr parameter.  The fi_addr parameter
acts as input/output in this case.  When the FI_AV_USER_ID flag is passed
to any of the insert calls, the caller must specify an fi_addr_t identifier
value to associate with each address.  The provider will record that
identifier and use it where required as part of any completion event.  Note
that the output from the AV insertion call is unchanged.  The provider will
return an fi_addr_t value that maps to each address, and that value must be
used for all data transfer operations.

# PEER GROUPS

Peer groups provide a direct mapping to HPC and AI communicator constructs.

The addresses in an AV represent the full set of peers that a local process
may communicate with.  A peer group conceptually represents a subset of
those peers.  A peer group may be used to identify peers working on a common
task, which need their communication logically separated from other traffic.
Peer groups are not a security mechanism, but instead help separate data.
A given peer may belong to 0 or more peer groups,
with no limit placed on how many peers can belong to a single peer group.

Peer groups are identified using an integer value, known as a group id.
Group id's are selected by the user and conveyed as part of an fi_addr_t
value.  The management of a group id and it's relationship to addresses
inserted into an AV is directly controlled by the user.  When enabled,
sent messages are marked as belonging to a specific peer group, and posted
receive buffers must have a matching group id to receive the data.

Users are responsible for selecting a valid peer group id, subject to the
limitation negotiated using the domain attribute max_group_id.  The group
id of an fi_addr_t may be set using the fi_group_addr() function.

## fi_group_addr

This function is used to set the group ID portion of an fi_addr_t.

# RETURN VALUES

Insertion calls will return the number of addresses that were successfully
inserted.  In the case of failure, the return value will be less than the
number of addresses that were specified.  Providers may abort inserting
addresses on the the insertion failure.  The fi_addr buffer associated with
a failed or aborted insertion will be set to FI_ADDR_NOTAVAIL.

All other calls return 0 on success, or a negative value corresponding to
fabric errno on error.  Fabric errno values are defined in `rdma/fi_errno.h`.

# SEE ALSO

[`fi_getinfo`(3)](fi_getinfo.3.html),
[`fi_endpoint`(3)](fi_endpoint.3.html),
[`fi_domain`(3)](fi_domain.3.html),
[`fi_eq`(3)](fi_eq.3.html)
