/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2021 Hewlett Packard Enterprise Development LP
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>

#include <ofi.h>

#include "cxip.h"

#define CXIP_DBG(...) _CXIP_DBG(FI_LOG_FABRIC, __VA_ARGS__)
#define CXIP_WARN(...) _CXIP_WARN(FI_LOG_FABRIC, __VA_ARGS__)


#define	CHUNK_SIZE	4096
#define	CHUNK_MASK	(CHUNK_SIZE-1)

static CURLM *cxip_curlm = NULL;

/**
 * Expandable buffer that can receive data in arbitrary-sized chunks.
 *
 */
struct curl_buffer {
	char *buffer;
	size_t size;
	size_t offset;
};

/**
 * Allocate an expandable CURL buffer.
 *
 * This expands as necessary to accommodate the data, which may be delivered in
 * chunks over the network. If you know in advance the approximate size of the
 * return data on a large transfer, you can avoid repeated calls to realloc().
 *
 * @param rsp_init_size : initial size of buffer area (> 0), default 4k
 *
 * @return struct curl_buffer* : returned CURL buffer
 */
static inline struct curl_buffer *init_curl_buffer(size_t rsp_init_size)
{
	struct curl_buffer *buf;

	if (rsp_init_size == 0)
		rsp_init_size = 4096;
	buf = calloc(1, sizeof(*buf));
	if (!buf)
		return NULL;

	buf->buffer = malloc(rsp_init_size);
	if (!buf->buffer) {
		free(buf);
		buf = NULL;
	}

	return buf;
}

/**
 * Free a curl_buffer and all its data.
 *
 * @param buf : curl buffer to delete
 */
static inline void free_curl_buffer(struct curl_buffer *buf)
{
	if (buf)
		free(buf->buffer);
	free(buf);
}

/**
 * Curl is curl-centric, facing the application (not the server). For Curl, a
 * "write" is a write to application memory. This is the function that fills
 * the user's curl buffer with data returned by the server.
 *
 * Buffer expands as needed to accommodate data. Note that this means the buffer
 * itself must be treated as uninitialized memory beyond buf->offset (see
 * realloc()).
 *
 * If the return value does not match the number of bytes requested, it will
 * abort the transfer and the curl function will return CURLE_WRITE_ERROR.
 *
 * @param curl_rcvd : poiter to data received from server
 * @param size : size of member
 * @param nmemb : number of members
 * @param userp : (void *)-cast curl_buffer
 *
 * @return ssize_t : number of bytes added
 */
static size_t write_callback(void *curl_rcvd, size_t size, size_t nmemb,
			     void *userp)
{
	struct curl_buffer *curl_buf = (struct curl_buffer *)userp;
	size_t sz = size * nmemb;
	size_t need = curl_buf->offset + sz;

	if (need >= curl_buf->size) {
		curl_buf->size = (need + CHUNK_MASK) & ~CHUNK_MASK;
		curl_buf->buffer = realloc(curl_buf->buffer, curl_buf->size);
		if (!curl_buf->buffer)
			return 0;
	}
	memcpy(&curl_buf->buffer[curl_buf->offset], curl_rcvd, sz);

	curl_buf->offset += sz;
	return sz;
}

/**
 * Globally initialize this module.
 */
void cxip_curl_init(void)
{
	CURLcode res;

	res = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (res == CURLE_OK) {
		cxip_curlm = curl_multi_init();
		if (! cxip_curlm)
			curl_global_cleanup();
	}
}

/**
 * Globally terminate this module.
 */
void cxip_curl_term(void)
{
	if (cxip_curlm) {
		curl_multi_cleanup(cxip_curlm);
		cxip_curlm = NULL;
	}
	curl_global_cleanup();
}

/**
 * Return a name for an opcode.
 *
 * @param op            : curl operation
 * @return const char*  : printable name for curl operation
 */
const char *cxip_curl_opname(enum curl_ops op)
{
	static char * const curl_opnames[] = {
		"GET",
		"PUT",
		"POST",
		"PATCH",
		"DELETE",
	};
	return (op >= 0 && op < CURL_MAX) ? curl_opnames[op] : NULL;
}

/**
 * Dispatch a CURL request.
 *
 * This is a general-purpose CURL multi (async) JSON format curl request.
 *
 * Note that this only dispatches the request. cxip_curl_progress() must be
 * called to progress the dispatched operations and retrieve data.
 *
 * @param endpoint      : HTTP server endpoint address
 * @param request       : JSON-formatted request
 * @param rsp_init_size : initial size of response buffer (can be 0)
 * @param op            : curl operation
 * @param verbose       : use to display sent HTTP headers
 *
 * @return int          : 0 on success, -1 on failure
 */
int cxip_curl_perform(const char *endpoint, const char *request,
		      size_t rsp_init_size, enum curl_ops op, bool verbose)
{
	struct cxip_curl_handle *h;
	struct curl_slist *headers;
	CURLMcode mres;
	CURL *curl;
	int running;

	h = calloc(1, sizeof(*h));
	if (!h)
		goto done;

	/* libcurl is fussy about NULL requests */
	h->endpoint = strdup(endpoint);
	if (!h->endpoint)
		goto done;
	h->request = strdup(request ? request : "");
	if (!h->request)
		goto done;
	h->response = NULL;
	h->recv = (void *)init_curl_buffer(rsp_init_size);
	if (!h->recv)
		goto done;

	curl = curl_easy_init();
	if (!curl) {
		CXIP_WARN("curl_easy_init() failed\n");
		goto done;
	}

	/* HTTP 1.1 assumed */
	headers = NULL;
	headers = curl_slist_append(headers, "Expect:");
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "charset: utf-8");
	h->headers = (void *)headers;

	curl_easy_setopt(curl, CURLOPT_URL, h->endpoint);
	if (op == CURL_GET) {
		curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	} else {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, h->request);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
				 strlen(h->request));
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, h->recv);
	curl_easy_setopt(curl, CURLOPT_PRIVATE, (void *)h);
	curl_easy_setopt(curl, CURLOPT_VERBOSE, verbose);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cxip_curl_opname(op));

	curl_multi_add_handle(cxip_curlm, curl);
	mres = curl_multi_perform(cxip_curlm, &running);
	if (mres != CURLM_OK) {
		CXIP_WARN("curl_multi_perform() failed: %s\n",
			  curl_multi_strerror(mres));
		goto done;
	}
	return 0;

done:
	CXIP_WARN("cxip_curl_perform() failed\n");
	cxip_curl_free(h);
	return -1;
}

/**
 * Progress the CURL requests.
 *
 * This progresses concurrent CURL requests, and returns the count of
 * http operations still running, or -1 on an error. It signals completion
 * of a single operation by returning a non-NULL handle.
 *
 * Repeated calls will return additional completions.
 *
 * Call cxip_curl_free() on the handle to recover resources.
 *
 * The handle contains the following public fields:
 *
 * - status   = HTTP status of the op, or 0 if the endpoint could not be reached
 * - endpoint = copy of the endpoint address supplied for the post
 * - request  = copy of the JSON request data supplied for the post
 * - response = pointer to the JSON response returned by the endpoint
 *
 * @param handle   : returned handle, NULL if nothing to process
 *
 * @return int     : count of running + unprocessed results
 */
int cxip_curl_progress(struct cxip_curl_handle **handle)
{
	struct CURLMsg *msg;
	CURLMcode mres;
	CURLcode res;
	int running;
	int messages;
	long status;
	struct curl_buffer *recv;

	*handle = NULL;

	/* running returns the number of curls running */
	mres = curl_multi_perform(cxip_curlm, &running);
	if (mres != CURLM_OK) {
		CXIP_WARN("curl_multi_perform() failed: %s\n",
			  curl_multi_strerror(mres));
		running = -1;
		goto done;
	}

	/* messages returns the number of additional curls finished */
	msg = curl_multi_info_read(cxip_curlm, &messages);
	if (!msg || msg->msg != CURLMSG_DONE)
		goto done;

	/* retrieve our handle from the private pointer */
	res = curl_easy_getinfo(msg->easy_handle,
				CURLINFO_PRIVATE, (char **)handle);
	if (res != CURLE_OK) {
		CXIP_WARN("curl_easy_getinfo(%s) failed: %s\n",
			"CURLINFO_PRIVATE",
			curl_easy_strerror(res));
		goto done;
	}

	/* retrieve the status code, should not fail */
	res = curl_easy_getinfo(msg->easy_handle,
				CURLINFO_RESPONSE_CODE, &status);
	if (res != CURLE_OK) {
		CXIP_WARN("curl_easy_getinfo(%s) failed: %s\n",
			"CURLINFO_RESPONSE_CODE",
			curl_easy_strerror(res));
		/* continue, (*handle)->status should show zero */
	}

	/* we can recover resources now */
	curl_slist_free_all((struct curl_slist *)(*handle)->headers);
	curl_easy_cleanup(msg->easy_handle);
	(*handle)->headers = NULL;

	/* make sure response string is terminated */
	recv = (struct curl_buffer *)(*handle)->recv;
	recv->buffer[recv->offset] = 0;
	(*handle)->response = recv->buffer;
	(*handle)->status = status;

done:
	return running + messages;
}

/**
 * Free a handle returned by fi_curl_progress().
 *
 * @param handle : handle returned by cxip_curl_progress
 */
void cxip_curl_free(struct cxip_curl_handle *handle)
{
	free((void *)handle->endpoint);
	free((void *)handle->request);
	/* do not directly free handle->response (== handle->recv->buffer) */
	free_curl_buffer((struct curl_buffer *)handle->recv);
	free(handle);
}

/**
 * @brief Simplified search for JSON objects.
 *
 * Simplified object search using a descriptor like the following:
 * Example: "firstkey.secondkey.arraykey[3].thirdkey"
 *
 * The first character is '.' or '['. If omitted, it is assumed to be '.'.
 *
 * The appearance of '.' indicates that the current object is expected to be
 * a json_type_object, and the text that follows is a key within the object.
 *
 * The appearance of '[' must be part of a '[<size_t>]' construction, and
 * indicates that the current object is expected to be a json_type_array, and
 * the specified integer value is an index into the array.
 *
 * The descriptor allows you to dive into the structure and return the endpoint
 * of the dive in the returned jval pointer, and returns the type of this
 * endpoint object.
 *
 * Note that this is a convenience method, primarily for testing. Results are
 * undefined if the '.' or '[' or ']' characters appear in a key.
 *
 * Note that the returned jval is a json_object. You can use the following
 * libjson functions to directly extract values:
 *
 * - json_object_get_boolean()
 * - json_object_get_int()
 * - json_object_get_int64()
 * - json_object_get_uint64()
 * - json_object_get_double()
 * - json_object_get_string()
 *
 * Note also that these functions are used in the variants below.
 *
 * All memory is managed by json, so on 'put' of the head object, all memory is
 * recovered.
 *
 * This returns json_type_null on any error.
 *
 * @param desc - string descriptor of endpoint argument
 * @param jobj - starting object
 * @param jval - final endpoint object, or NULL
 * @return enum json_type - type of the endpoint object
 */
enum json_type cxip_json_extract(const char *desc, struct json_object *jobj,
				 struct json_object **jval)
{
	const char *beg;
	struct json_object *jo;
	enum json_type jt;

	*jval = NULL;

	beg = desc;
	jo = jobj;
	jt = json_object_get_type(jo);
	while (*beg) {
		if (*beg == '[') {
			/* expect "[<integer>]" */
			size_t idx = 0;
			size_t len;

			if (jt != json_type_array)
				return json_type_null;
			/* skip '[' and ensure index is not empty */
			if (*(++beg) == ']')
				return json_type_null;
			idx = strtoul(beg, (char **)&beg, 10);
			/* ensure strtol consumed index */
			if (*(beg++) != ']')
				return json_type_null;
			/* check index validity */
			len = json_object_array_length(jo);
			if (idx >= len)
				return json_type_null;
			/* get the indexed object and continue */
			jo = json_object_array_get_idx(jo, idx);
			jt = json_object_get_type(jo);
			continue;
		}
		if (beg == desc || *beg == '.') {
			/* expect ".key" */
			char key[256], *p = key;
			size_t len = sizeof(key);

			if (jt != json_type_object)
				return json_type_null;
			/* skip leading '.' */
			if (*beg == '.')
				beg++;
			/* copy key from descriptor to local storage */
			while (*beg && *beg != '.' && *beg != '[' && --len > 0)
				*p++ = *beg++;
			*p = 0;
			/* extract the associated value */
			if (!json_object_object_get_ex(jo, key, &jo))
				return json_type_null;
			jt = json_object_get_type(jo);
			continue;
		}
	}

	/* return the final object */
	*jval = jo;
	return jt;
}

/**
 * @brief Simplified search for JSON terminal type values.
 *
 * @param desc : search descriptor for cxip_json_extract()
 * @param jobj : starting object
 * @param val  : return value
 * @return int : 0 on success, -EINVAL on error
 */
int cxip_json_bool(const char *desc, struct json_object *jobj, bool *val)
{
	struct json_object *jval;
	if (json_type_boolean != cxip_json_extract(desc, jobj, &jval))
		return -EINVAL;
	*val = json_object_get_boolean(jval);
	return 0;
}

int cxip_json_int(const char *desc, struct json_object *jobj, int *val)
{
	struct json_object *jval;
	if (json_type_int != cxip_json_extract(desc, jobj, &jval))
		return -EINVAL;
	*val = json_object_get_int(jval);
	return 0;
}

int cxip_json_int64(const char *desc, struct json_object *jobj, int64_t *val)
{
	struct json_object *jval;
	if (json_type_int != cxip_json_extract(desc, jobj, &jval))
		return -EINVAL;
	*val = json_object_get_int64(jval);
	return 0;
}

int cxip_json_double(const char *desc, struct json_object *jobj, double *val)
{
	struct json_object *jval;
	if (json_type_double != cxip_json_extract(desc, jobj, &jval))
		return -EINVAL;
	*val = json_object_get_double(jval);
	return 0;
}

int cxip_json_string(const char *desc, struct json_object *jobj,
		     const char **val)
{
	struct json_object *jval;
	if (json_type_string != cxip_json_extract(desc, jobj, &jval))
		return -EINVAL;
	*val = json_object_get_string(jval);
	return 0;
}

/**
 * Build a request to create a multicast address.
 * PROTOTYPE
 *
 * Returned pointer must be freed after use.
 *
 * @param cfg           : sysenv configuration file on prototype
 * @param mcast_id      : mcast_id requested, 0 to allow configurator to choose
 * @param root_port_idx : index (rank) of the root node in the configuration
 *
 * @return char*        : JSON request
 */
static char *request_mcast_req(const char *cfg, unsigned int mcast_id,
			       unsigned root_port_idx)
{
	static const char * format =
		"{"
		"  'sysenv_cfg_path': '%s',"
		"  'params_ds': ["
		"    {"
		"      'mcast_id': %d,"
		"      'root_port_idx': %d"
		"    }"
		"  ]"
		"}";
	char *str;
	if (asprintf(&str, format, cfg, mcast_id, root_port_idx) <= 0)
		return NULL;
	single_to_double_quote(str);
	return str;
}

/**
 * Build a request to delete a multicast address.
 * PROTOTYPE
 *
 * @param id : id value returned with mcast creation
 *
 * @return const char* : JSON request
 */
static char *delete_mcast_req(int id)
{
	static const char * format = "{'id': %d}";
	char *str;
	if (asprintf(&str, format, id) <= 0)
		return NULL;
	single_to_double_quote(str);
	return str;
}

/**
 * Parse response from a configuration server for a multicast request.
 * PROTOTYPE
 *
 * @param response : response from configurator to be parsed
 * @param reqid    : 'id' value for mcast_id
 * @param mcast_id : multicast address
 * @param root_idx : index (rank) of the root node in the configuration
 *
 * @return int     : 0 success, -1 response is empty, -2 bad JSON
 */
static int mcast_parse(const char *response, int *reqid, int *mcast_id,
		       int *root_idx)
{
	/* Abstract structure:
	 *
	 * {"id": n, "tree_coll": [
	 *     {"mcast_id": n, "port_set_id": n, "root_port_idx": n },
	 *     {"mcast_id": n, "port_set_id": n, "root_port_idx": n }
	 *   ]
	 * }
	 *
	 * In practice, there will never be more than one mcast_id in the
	 * tree_coll array, because this POST will be issued exactly once per
	 * fi_join_collective() call, and the join operation will only generate
	 * one multicast ID.
	 *
	 * The simplified parser below notes that all of the tags are unique, so
	 * the actual structure is irrelevant. All of the values associated with
	 * tags of interest are integers. Thus, we can look for the tags, and
	 * the next token will be the associated (primitive) integer value.
	 */
	json_object *json_obj;

	if (!(json_obj = json_tokener_parse(response)))
		return -1;

	if (cxip_json_int("id", json_obj, reqid))
		return -1;
	if (cxip_json_int("tree_coll.mcast_id", json_obj, mcast_id))
		return -1;
	if (cxip_json_int("tree_coll.root_port_idx", json_obj, root_idx))
		return -1;

	return 0;
}

/**
 * Perform a CURL request to create a multicast address.
 *
 * @param endpoint      : server endpoint URL
 * @param cfg_path      : tree configuration path (prototype)
 * @param mcast_id      : requested address, or 0 for allocated
 * @param root_port_idx : requested root node index
 * @param verbose       : visibility of posted request
 *
 * @return int          : 0 on success, <0 on error
 */
int cxip_request_mcast(const char *endpoint, const char *cfg_path,
		       unsigned int mcast_id, unsigned int root_port_idx,
		       bool verbose)
{
	char *request;
	int ret = -1;

	request = request_mcast_req(cfg_path, mcast_id, root_port_idx);
	if (request) {
		ret = cxip_curl_perform(endpoint, request, 0, CURL_POST,
					verbose);
		free(request);
	}
	return ret;
}

/**
 * Perform a CURL request to delete a multicast address.
 *
 * @param endpoint     : server endpoint URL
 * @param reqid        : issued reqid from prior creation request
 * @param verbose      : visibility of posted request
 *
 * @return int         : 0 on success, <0 on error
 */
int cxip_delete_mcast(const char *endpoint, long reqid, bool verbose)
{
	char *request;
	int ret = -1;

	request = delete_mcast_req(reqid);
	if (request) {
		ret = cxip_curl_perform(endpoint, request, 0, CURL_DELETE,
					verbose);
		free(request);
	}
	return ret;
}

/**
 * Poll for completion of posted CURL requests.
 *
 * This returns -1 if post has not yet completed.
 *
 * This returns 0 if the server could not be reached.
 *
 * This otherwise returns the HTTP return code for the request.
 *   200 is a successful delete
 *   201 is a successful creation
 *
 * Other values represent other server-originated HTTP return codes.
 *
 * @param reqid        : server request identifier to use in DELETE
 * @param mcast_id     : server multicast address
 * @param root_idx     : server root node index
 *
 * @return int         : return code, see above
 */
int cxip_progress_mcast(int *reqid, int *mcast_id, int *root_idx)
{
	struct cxip_curl_handle *handle;
	long ret;

	ret = cxip_curl_progress(&handle);
	if (handle) {
		ret = handle->status;
		if (ret == 200) {
			// delete succeeded
		} else if (ret == 201) {
			// post succeeded
			mcast_parse(handle->response, reqid, mcast_id, root_idx);
		} else if (ret != 0) {
			CXIP_WARN("Server error %ld\n", ret);
		} else {
			CXIP_WARN("Unable to connect with %s\n",
				handle->endpoint);
		}
		cxip_curl_free(handle);
	}
	return (int)ret;
}
