#define PURPLE_PLUGINS


#include "hangouts_connection.h"


#include <stdlib.h>
#include <stdio.h>

#include "cipher.h"
#include "debug.h"
#ifdef _WIN32
#include "win32/win32dep.h"
#endif

#include "hangouts_pblite.h"
#include "hangouts_json.h"
#include "hangouts.pb-c.h"


void
hangouts_process_data_chunks(const gchar *data, gsize len)
{
	JsonArray *chunks;
	guint i, num_chunks;
	
	chunks = json_decode_array(data, len);
	
	for (i = 0, num_chunks = json_array_get_length(chunks); i < num_chunks; i++) {
		JsonArray *chunk;
		JsonArray *array;
		JsonNode *array0;
		
		chunk = json_array_get_array_element(chunks, i);
		
		array = json_array_get_array_element(chunk, 1);
		array0 = json_array_get_element(array, 0);
		if (JSON_NODE_HOLDS_VALUE(array0)) {
			//probably a nooooop
			if (g_strcmp0(json_node_get_string(array0), "noop") == 0) {
				//A nope ninja delivers a wicked dragon kick
#ifdef DEBUG
				printf("noop\n");
#endif
			}
		} else {
			const gchar *p = json_object_get_string_member(json_node_get_object(array0), "p");
			JsonObject *wrapper = json_decode_object(p, -1);
			
			if (wrapper == NULL) {
				continue;
			}
			
			if (json_object_has_member(wrapper, "3")) {
				const gchar *new_client_id = json_object_get_string_member(json_object_get_object_member(wrapper, "3"), "2");
				purple_debug_info("hangouts", "Received new client_id: %s\n", new_client_id);
				//TODO set the client_id on the connection
			}
			if (json_object_has_member(wrapper, "2")) {
				const gchar *wrapper22 = json_object_get_string_member(json_object_get_object_member(wrapper, "2"), "2");
				JsonArray *pblite_message = json_decode_array(wrapper22, -1);

				if (pblite_message == NULL) {
#ifdef DEBUG
					printf("bad wrapper22 %s\n", wrapper22);
#endif
					json_object_unref(wrapper);
					continue;
				}
				
				//cbu == ClientBatchUpdate
				if (g_strcmp0(json_array_get_string_element(pblite_message, 0), "cbu") == 0) {
					BatchUpdate batch_update = BATCH_UPDATE__INIT;
					
#ifdef DEBUG
					printf("----------------------\n");
#endif
					pblite_decode((ProtobufCMessage *) &batch_update, pblite_message, TRUE);
#ifdef DEBUG
					printf("======================\n");
					printf("Is valid? %s\n", protobuf_c_message_check((ProtobufCMessage *) &batch_update) ? "Yes" : "No");
					printf("======================\n");
					JsonArray *debug = pblite_encode((ProtobufCMessage *) &batch_update);
					JsonNode *node = json_node_new(JSON_NODE_ARRAY);
					json_node_take_array(node, debug);
					gchar *json = json_encode(node, NULL);
					printf("Old: %s\nNew: %s\n", wrapper22, json);
					
					
					pblite_decode((ProtobufCMessage *) &batch_update, debug, TRUE);
					debug = pblite_encode((ProtobufCMessage *) &batch_update);
					json_node_take_array(node, debug);
					gchar *json2 = json_encode(node, NULL);
					printf("Mine1: %s\nMine2: %s\n", json, json2);
					
					g_free(json);
					g_free(json2);
					printf("----------------------\n");
#endif
				}
				
				json_array_unref(pblite_message);
			}
			
			json_object_unref(wrapper);
		}
	}
	
	json_array_unref(chunks);
}

static int
read_all(int fd, void *buf, size_t len)
{
	unsigned int rs = 0;
	while(rs < len)
	{
		int rval = read(fd, buf + rs, len - rs);
		if (rval == 0)
			break;
		if (rval < 0)
			return rval;

		rs += rval;
	}
	return rs;
}

void
hangouts_process_channel(int fd)
{
	gsize len, lenpos = 0;
	gchar len_str[256];
	gchar *chunk;
	
	while(read(fd, len_str + lenpos, 1) > 0) {
		//read up until \n
		if (len_str[lenpos] == '\n') {
			//convert to int, use as length of string to read
			len_str[lenpos] = '\0';
#ifdef DEBUG
			printf("len_str is %s\n", len_str);
#endif
			len = atoi(len_str);
			
			chunk = g_new(gchar, len * 2);
			//XX - could be a utf-16 length*2 though, so read up until \n????
			
			if (read_all(fd, chunk, len) > 0) {
				//throw chunk to hangouts_process_data_chunks
				hangouts_process_data_chunks(chunk, len);
			}
			
			g_free(chunk);
			
			lenpos = 0;
		} else {
			lenpos = lenpos + 1;
		}
	}
}

void
hangouts_process_channel_buffer(HangoutsAccount *ha)
{
	const gchar *bufdata;
	gsize remaining;
	gchar *len_end;
	gchar *len_str;
	guint64 len;
	
	g_return_if_fail(ha);
	g_return_if_fail(ha->channel_buffer);
	
	do {
		bufdata = purple_circular_buffer_get_output(ha->channel_buffer);
		remaining = purple_circular_buffer_get_max_read(ha->channel_buffer);
		
		len_end = g_strstr_len(bufdata, remaining, "\n");
		if (len_end == NULL) {
			// Not enough data to read
			return;
		}
		len_str = g_strndup(bufdata, len_end - bufdata);
		len = g_ascii_strtoull(len_str, NULL, 10);
		g_free(len_str);
		
		bufdata = len_end + 1;
		remaining = remaining - (len_end - bufdata) - 1;
		
		if (len > remaining) {
			// Not enough data to read
			return;
		}
		
		hangouts_process_data_chunks(bufdata, len);
		
		purple_circ_buffer_mark_read(ha->channel_buffer, len);
		remaining = purple_circular_buffer_get_max_read(ha->channel_buffer);
		
	} while (remaining);
}

static void
hangouts_set_auth_headers(HangoutsAccount *ha, PurpleHttpRequest *request)
{
	gint64 mstime;
	gchar *mstime_str;
	GTimeVal time;
	PurpleCipherContext *sha1_ctx;
	gchar sha1[41];
	gchar *sapisid_cookie;
	
	g_get_current_time(&time);
	mstime = (((gint64) time.tv_sec) * 1000) + (time.tv_usec / 1000);
	mstime_str = g_strdup_printf("%" G_GINT64_FORMAT, mstime);
	sapisid_cookie = purple_http_cookie_jar_get(ha->cookie_jar, "SAPISID");
	
	sha1_ctx = purple_cipher_context_new(purple_ciphers_find_cipher("sha1"), NULL);
	purple_cipher_context_append(sha1_ctx, (guchar *) mstime_str, strlen(mstime_str));
	purple_cipher_context_append(sha1_ctx, (guchar *) " ", 1);
	purple_cipher_context_append(sha1_ctx, (guchar *) sapisid_cookie, strlen(sapisid_cookie));
	purple_cipher_context_append(sha1_ctx, (guchar *) " ", 1);
	purple_cipher_context_append(sha1_ctx, (guchar *) HANGOUTS_PBLITE_XORIGIN_URL, strlen(HANGOUTS_PBLITE_XORIGIN_URL));
	purple_cipher_context_digest_to_str(sha1_ctx, 41, sha1, NULL);
	purple_cipher_context_destroy(sha1_ctx);
	
	purple_http_request_header_set_printf(request, "Authorization", "SAPISIDHASH %s_%s", mstime_str, sha1);
	purple_http_request_header_set(request, "X-Origin", HANGOUTS_PBLITE_XORIGIN_URL);
	purple_http_request_header_set(request, "X-Goog-AuthUser", "0");
	
	g_free(sapisid_cookie);
}


static gboolean
hangouts_longpoll_request_content(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, const gchar *buffer, size_t offset, size_t length, gpointer user_data)
{
	HangoutsAccount *ha = user_data;
	
	if (purple_http_response_get_error(response) != NULL) {
		return FALSE;
	}
	
	purple_circular_buffer_append(ha->channel_buffer, buffer, length);
	
	hangouts_process_channel_buffer(ha);
	
	return TRUE;
}

void
hangouts_longpoll_request(HangoutsAccount *ha)
{
	PurpleHttpRequest *request;
	GString *url;

	
	url = g_string_new(HANGOUTS_CHANNEL_URL_PREFIX "channel/bind" "?");
	g_string_append(url, "VER=8&");           // channel protocol version
	g_string_append_printf(url, "gsessionid=%s&", purple_url_encode(ha->gsessionid_param));
	g_string_append(url, "RID=rpc&");         // request identifier
	g_string_append(url, "t=1&");             // trial
	g_string_append_printf(url, "SID=%s&", purple_url_encode(ha->sid_param));  // session ID
	g_string_append(url, "CI=0&");            // 0 if streaming/chunked requests should be used
	g_string_append(url, "ctype=hangouts&");  // client type
	g_string_append(url, "TYPE=xmlhttp&");    // type of request
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_url(request, url->str);
	purple_http_request_set_timeout(request, -1);  // to infinity and beyond!
	purple_http_request_set_response_writer(request, hangouts_longpoll_request_content, ha);
	purple_http_request_set_keepalive_pool(request, ha->channel_keepalive_pool);
	
	hangouts_set_auth_headers(ha, request);
	
	purple_http_request(ha->pc, request, NULL, ha);
	
	g_string_free(url, TRUE);
}



void
hangouts_fetch_channel_sid(HangoutsAccount *ha)
{
	g_free(ha->sid_param);
	g_free(ha->gsessionid_param);
	ha->sid_param = NULL;
	ha->gsessionid_param = NULL;
	
	hangouts_send_maps(ha, NULL);
}

static void
hangouts_send_maps_cb(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	/*111
	 * [
	 * [0,["c","<sid>","",8]],
	 * [1,[{"gsid":"<gsid>"}]]
	 * ]
	 */
	JsonNode *node;
	HangoutsAccount *ha = user_data;
	const gchar *res_raw;
	size_t res_len;
	gchar *gsid;
	gchar *sid;

	res_raw = purple_http_response_get_data(response, &res_len);
	res_raw = g_strstr_len(res_raw, res_len, "\n");
	res_raw++;
	node = json_decode(res_raw, -1);
	sid = hangouts_json_path_query_string(node, "$[0][1][1]", NULL);
	gsid = hangouts_json_path_query_string(node, "$[1][1][0].gsid", NULL);

	ha->sid_param = sid;
	ha->gsessionid_param = gsid;

	json_node_free (node);
	
	hangouts_longpoll_request(ha);
}

void
hangouts_send_maps(HangoutsAccount *ha, JsonArray *map_list)
{
	PurpleHttpRequest *request;
	GString *url, *postdata;
	guint map_list_len, i;
	
	url = g_string_new(HANGOUTS_CHANNEL_URL_PREFIX "channel/bind" "?");
	g_string_append(url, "VER=8&");           // channel protocol version
	g_string_append(url, "RID=81188&");       // request identifier
	g_string_append(url, "ctype=hangouts&");  // client type
	if (ha->gsessionid_param)
		g_string_append_printf(url, "gsessionid=%s&", purple_url_encode(ha->gsessionid_param));
	if (ha->sid_param)
		g_string_append_printf(url, "SID=%s&", purple_url_encode(ha->sid_param));  // session ID
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_url(request, url->str);
	purple_http_request_set_method(request, "POST");
	purple_http_request_header_set(request, "Content-Type", "application/x-www-form-urlencoded");
	purple_http_request_header_set_printf(request, "Authorization", "Bearer %s", ha->access_token);
	
	hangouts_set_auth_headers(ha, request);
	
	postdata = g_string_new(NULL);
	if (map_list != NULL) {
		for(i = 0, map_list_len = json_array_get_length(map_list); i < map_list_len; i++) {
			JsonObject *obj = json_array_get_object_element(map_list, i);
			GList *members = json_object_get_members(obj);
			
			for (; members != NULL; members = members->next) {
				const gchar *member_name = members->data;
				JsonNode *value = json_object_get_member(obj, member_name);
				gchar *json = json_encode(value, NULL);
				
				g_string_append_printf(postdata, "req%u_%s=", i, purple_url_encode(member_name));
				g_string_append_printf(postdata, "%s&", purple_url_encode(json));
				
				g_free(json);
			}
		}
	}
	purple_http_request_set_contents(request, postdata->str, postdata->len);

	purple_http_request(ha->pc, request, hangouts_send_maps_cb, ha);
	
	g_string_free(postdata, TRUE);
	g_string_free(url, TRUE);	
}

void
hangouts_add_channel_services(HangoutsAccount *ha)
{
	JsonArray *map_list = json_array_new();
	JsonObject *obj = json_object_new();
	
	json_object_set_string_member(obj, "p", "{\"3\":{\"1\":{\"1\":\"babel\"}}}");
	json_array_add_object_element(map_list, obj);
	hangouts_send_maps(ha, map_list);
	
	json_array_unref(map_list);
}


typedef struct {
	HangoutsAccount *ha;
	HangoutsPbliteResponseFunc callback;
	ProtobufCMessage *response_message;
	gpointer user_data;
} LazyPblistRequestStore;

static void
hangouts_pblite_request_cb(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	LazyPblistRequestStore *request_info = user_data;
	HangoutsAccount *ha = request_info->ha;
	HangoutsPbliteResponseFunc callback = request_info->callback;
	gpointer real_user_data = request_info->user_data;
	ProtobufCMessage *response_message = request_info->response_message;
	JsonArray *response_array;
	const gchar *raw_response;
	gsize response_len;
	
	raw_response = purple_http_response_get_data(response, &response_len);
	response_array = json_decode_array(raw_response, response_len);
	pblite_decode(response_message, response_array, /*Ignore First Item= */TRUE);
	
	purple_debug_info("hangouts", "A '%s' says '%s'\n", response_message->descriptor->name, json_array_get_string_element(response_array, 0));
	
	callback(ha, response_message, real_user_data);
	
	json_array_unref(response_array);
	g_free(request_info);
}

void
hangouts_pblite_request(HangoutsAccount *ha, const gchar *endpoint, ProtobufCMessage *request_message, HangoutsPbliteResponseFunc callback, ProtobufCMessage *response_message, gpointer user_data)
{
	PurpleHttpRequest *request;
	JsonArray *request_array;
	gsize request_len;
	gchar *request_data;
	LazyPblistRequestStore *request_info = g_new0(LazyPblistRequestStore, 1);
	
	request_array = pblite_encode(request_message);
	request_data = json_encode_array(request_array, &request_len);
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_url_printf(request, "https://clients6.google.com/chat/v1/%s", endpoint);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_method(request, "POST");
	purple_http_request_header_set(request, "Content-Type", "application/json+protobuf");
	purple_http_request_set_contents(request, request_data, request_len);
	
	request_info->ha = ha;
	request_info->callback = callback;
	request_info->response_message = response_message;
	request_info->user_data = user_data;
	
	
	hangouts_set_auth_headers(ha, request);
	
	purple_http_request(ha->pc, request, hangouts_pblite_request_cb, request_info);
	
	g_free(request_data);
	json_array_unref(request_array);
}


void
hangouts_pblite_send_chat_message(HangoutsAccount *ha, SendChatMessageRequest *request, HangoutsPbliteChatMessageResponseFunc callback, gpointer user_data)
{
	SendChatMessageResponse response;
	
	send_chat_message_response__init(&response);	
	hangouts_pblite_request(ha, "conversations/sendchatmessage", (ProtobufCMessage *)request, (HangoutsPbliteResponseFunc)callback, (ProtobufCMessage *)&response, user_data);
}