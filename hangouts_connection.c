/*
 * Hangouts Plugin for libpurple/Pidgin
 * Copyright (c) 2015-2016 Eion Robb, Mike Ruprecht
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define PURPLE_PLUGINS


#include "hangouts_connection.h"


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "debug.h"
#include "request.h"

#include "hangouts_pblite.h"
#include "hangouts_json.h"
#include "hangouts.pb-c.h"
#include "hangouts_conversation.h"

#include "gmail.pb-c.h"

void
hangouts_process_data_chunks(HangoutsAccount *ha, const gchar *data, gsize len)
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
				
				g_free(ha->client_id);
				ha->client_id = g_strdup(new_client_id);
				
				hangouts_add_channel_services(ha);
				hangouts_set_active_client(ha->pc);
				hangouts_set_status(ha->account, purple_account_get_active_status(ha->account));
			}
			if (json_object_has_member(wrapper, "2")) {
				const gchar *wrapper22 = json_object_get_string_member(json_object_get_object_member(wrapper, "2"), "2");
				JsonArray *pblite_message = json_decode_array(wrapper22, -1);
				const gchar *message_type;

				if (pblite_message == NULL) {
#ifdef DEBUG
					printf("bad wrapper22 %s\n", wrapper22);
#endif
					json_object_unref(wrapper);
					continue;
				}
				
				message_type = json_array_get_string_element(pblite_message, 0);
				
				//cbu == ClientBatchUpdate
				if (purple_strequal(message_type, "cbu")) {
					BatchUpdate batch_update = BATCH_UPDATE__INIT;
					guint j;
					
#ifdef DEBUG
					printf("----------------------\n");
#endif
					pblite_decode((ProtobufCMessage *) &batch_update, pblite_message, TRUE);
#ifdef DEBUG
					printf("======================\n");
					printf("Is valid? %s\n", protobuf_c_message_check((ProtobufCMessage *) &batch_update) ? "Yes" : "No");
					printf("======================\n");
					printf("CBU %s", pblite_dump_json((ProtobufCMessage *)&batch_update));
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
					for(j = 0; j < batch_update.n_state_update; j++) {
						purple_signal_emit(purple_connection_get_protocol(ha->pc), "hangouts-received-stateupdate", ha->pc, batch_update.state_update[j]);
					}
				} else if (purple_strequal(message_type, "n_nm")) {
					GmailNotification gmail_notification = GMAIL_NOTIFICATION__INIT;
					const gchar *username = json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(wrapper, "2"), "1"), "2");
					
					pblite_decode((ProtobufCMessage *) &gmail_notification, pblite_message, TRUE);
					purple_signal_emit(purple_connection_get_protocol(ha->pc), "hangouts-gmail-notification", ha->pc, username, &gmail_notification);
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
				hangouts_process_data_chunks(NULL, chunk, len);
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
	gsize bufsize;
	gchar *len_end;
	gchar *len_str;
	guint len_len; //len len len len len len len len len
	gsize len;
	
	g_return_if_fail(ha);
	g_return_if_fail(ha->channel_buffer);
	
	while (ha->channel_buffer->len) {
		bufdata = (gchar *) ha->channel_buffer->data;
		bufsize = ha->channel_buffer->len;
		
		len_end = g_strstr_len(bufdata, bufsize, "\n");
		if (len_end == NULL) {
			// Not enough data to read
			if (purple_debug_is_verbose()) {
				purple_debug_info("hangouts", "Couldn't find length of chunk\n");
			}
			return;
		}
		len_len = len_end - bufdata;
		len_str = g_strndup(bufdata, len_len);
		len = (gsize) atoi(len_str);
		g_free(len_str);
		
		// Len was 0 ?  Must have been a bad read :(
		g_return_if_fail(len);
		
		bufsize = bufsize - len_len - 1;
		
		if (len > bufsize) {
			// Not enough data to read
			if (purple_debug_is_verbose()) {
				purple_debug_info("hangouts", "Couldn't read %" G_GSIZE_FORMAT " bytes when we only have %" G_GSIZE_FORMAT "\n", len, bufsize);
			}
			return;
		}
		
		hangouts_process_data_chunks(ha, bufdata + len_len + 1, len);
		
		g_byte_array_remove_range(ha->channel_buffer, 0, len + len_len + 1);
		
	}
}

static void
hangouts_set_auth_headers(HangoutsAccount *ha, PurpleHttpRequest *request)
{
	gint64 mstime;
	gchar *mstime_str;
	GTimeVal time;
	GChecksum *hash;
	const gchar *sha1;
	gchar *sapisid_cookie;
	
	g_get_current_time(&time);
	mstime = (((gint64) time.tv_sec) * 1000) + (time.tv_usec / 1000);
	mstime_str = g_strdup_printf("%" G_GINT64_FORMAT, mstime);
	sapisid_cookie = purple_http_cookie_jar_get(ha->cookie_jar, "SAPISID");
	
	hash = g_checksum_new(G_CHECKSUM_SHA1);
	g_checksum_update(hash, (guchar *) mstime_str, strlen(mstime_str));
	g_checksum_update(hash, (guchar *) " ", 1);
	if (sapisid_cookie && *sapisid_cookie) {
		// Should we just bail out if we dont have the cookie?
		g_checksum_update(hash, (guchar *) sapisid_cookie, strlen(sapisid_cookie));
	}
	g_checksum_update(hash, (guchar *) " ", 1);
	g_checksum_update(hash, (guchar *) HANGOUTS_PBLITE_XORIGIN_URL, strlen(HANGOUTS_PBLITE_XORIGIN_URL));
	sha1 = g_checksum_get_string(hash);
	
	purple_http_request_header_set_printf(request, "Authorization", "SAPISIDHASH %s_%s", mstime_str, sha1);
	purple_http_request_header_set(request, "X-Origin", HANGOUTS_PBLITE_XORIGIN_URL);
	purple_http_request_header_set(request, "X-Goog-AuthUser", "0");
	
	g_free(sapisid_cookie);
	g_free(mstime_str);
	g_checksum_free(hash);
}


static gboolean
hangouts_longpoll_request_content(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, const gchar *buffer, size_t offset, size_t length, gpointer user_data)
{
	HangoutsAccount *ha = user_data;
	
	ha->last_data_received = time(NULL);
	
	if (!purple_http_response_is_successful(response)) {
		purple_debug_error("hangouts", "longpoll_request_content had error: '%s'\n", purple_http_response_get_error(response));
		return FALSE;
	}
	
	g_byte_array_append(ha->channel_buffer, (guint8 *) buffer, length);
	
	hangouts_process_channel_buffer(ha);
	
	return TRUE;
}

static void
hangouts_longpoll_request_closed(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	HangoutsAccount *ha = user_data;
	
	if (!PURPLE_IS_CONNECTION(purple_http_conn_get_purple_connection(http_conn))) {
		return;
	}
	
	if (ha->channel_watchdog) {
		g_source_remove(ha->channel_watchdog);
		ha->channel_watchdog = 0;
	}
	
	// remaining data 'should' have been dealt with in hangouts_longpoll_request_content
	g_byte_array_free(ha->channel_buffer, TRUE);
	ha->channel_buffer = g_byte_array_sized_new(HANGOUTS_BUFFER_DEFAULT_SIZE);
	
	if (purple_http_response_get_error(response) != NULL) {
		//TODO error checking
		purple_debug_error("hangouts", "longpoll_request_closed %d %s\n", purple_http_response_get_code(response), purple_http_response_get_error(response));
		hangouts_fetch_channel_sid(ha);
	} else {
		hangouts_longpoll_request(ha);
	}
}

static gboolean
channel_watchdog_check(gpointer data)
{
	PurpleConnection *pc = data;
	HangoutsAccount *ha;
	PurpleHttpConnection *conn;
	
	if (PURPLE_IS_CONNECTION(pc)) {
		ha = purple_connection_get_protocol_data(pc);
		conn = ha->channel_connection;
		
		if (ha->last_data_received && ha->last_data_received < (time(NULL) - 60)) {
			// should have been something within the last 60 seconds
			purple_http_conn_cancel(conn);
			ha->last_data_received = 0;
		}
		
		if (!purple_http_conn_is_running(conn)) {
			hangouts_longpoll_request(ha);
		}
		
		return TRUE;
	}
	
	return FALSE;
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
	
	ha->channel_connection = purple_http_request(ha->pc, request, hangouts_longpoll_request_closed, ha);
	
	g_string_free(url, TRUE);
	
	if (ha->channel_watchdog) {
		g_source_remove(ha->channel_watchdog);
	}
	ha->channel_watchdog = g_timeout_add_seconds(1, channel_watchdog_check, ha->pc);
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
	gchar *json_start;
	size_t res_len;
	gchar *gsid;
	gchar *sid;
	
	if (purple_http_response_get_error(response) != NULL) {
		purple_connection_error(ha->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, purple_http_response_get_error(response));
		return;
	}

	res_raw = purple_http_response_get_data(response, &res_len);
	json_start = g_strstr_len(res_raw, res_len, "\n");
	if (json_start == NULL) {
		purple_connection_error(ha->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Blank maps response");
		return;
	}
	*json_start = '\0';
	json_start++;
	node = json_decode(json_start, atoi(res_raw));
	sid = hangouts_json_path_query_string(node, "$[0][1][1]", NULL);
	gsid = hangouts_json_path_query_string(node, "$[1][1][0].gsid", NULL);

	if (sid != NULL) {
		g_free(ha->sid_param);
		ha->sid_param = sid;
	}
	if (gsid != NULL) {
		g_free(ha->gsessionid_param);
		ha->gsessionid_param = gsid;
	}

	json_node_free(node);
	
	hangouts_longpoll_request(ha);
}

void
hangouts_fetch_channel_sid(HangoutsAccount *ha)
{
	g_free(ha->sid_param);
	g_free(ha->gsessionid_param);
	ha->sid_param = NULL;
	ha->gsessionid_param = NULL;
	
	hangouts_send_maps(ha, NULL, hangouts_send_maps_cb);
}

void
hangouts_send_maps(HangoutsAccount *ha, JsonArray *map_list, PurpleHttpCallback send_maps_callback)
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
	
	hangouts_set_auth_headers(ha, request);
	
	postdata = g_string_new(NULL);
	if (map_list != NULL) {
		map_list_len = json_array_get_length(map_list);
		g_string_append_printf(postdata, "count=%u&", map_list_len);
		g_string_append(postdata, "ofs=0&");
		for(i = 0; i < map_list_len; i++) {
			JsonObject *obj = json_array_get_object_element(map_list, i);
			GList *members = json_object_get_members(obj);
			GList *l;
			
			for (l = members; l != NULL; l = l->next) {
				const gchar *member_name = l->data;
				JsonNode *value = json_object_get_member(obj, member_name);
				
				g_string_append_printf(postdata, "req%u_%s=", i, purple_url_encode(member_name));
				g_string_append_printf(postdata, "%s&", purple_url_encode(json_node_get_string(value)));
			}

			g_list_free(members);
		}
	}
	purple_http_request_set_contents(request, postdata->str, postdata->len);

	purple_http_request(ha->pc, request, send_maps_callback, ha);
	purple_http_request_unref(request);
	
	g_string_free(postdata, TRUE);
	g_string_free(url, TRUE);	
}

void
hangouts_add_channel_services(HangoutsAccount *ha)
{
	JsonArray *map_list = json_array_new();
	JsonObject *obj;
	
	// TODO Work out what this is for
	obj = json_object_new();
	json_object_set_string_member(obj, "p", "{\"3\":{\"1\":{\"1\":\"tango_service\"}}}");
	json_array_add_object_element(map_list, obj);
	
	// This is for the chat messages
	obj = json_object_new();
	json_object_set_string_member(obj, "p", "{\"3\":{\"1\":{\"1\":\"babel\"}}}");
	json_array_add_object_element(map_list, obj);
	
	// This is for the presence updates
	obj = json_object_new();
	json_object_set_string_member(obj, "p", "{\"3\":{\"1\":{\"1\":\"babel_presence_last_seen\"}}}");
	json_array_add_object_element(map_list, obj);
	
	// TODO Work out what this is for
	obj = json_object_new();
	json_object_set_string_member(obj, "p", "{\"3\":{\"1\":{\"1\":\"hangout_invite\"}}}");
	json_array_add_object_element(map_list, obj);
	
	obj = json_object_new();
	json_object_set_string_member(obj, "p", "{\"3\":{\"1\":{\"1\":\"gmail\"}}}");
	json_array_add_object_element(map_list, obj);
	
	hangouts_send_maps(ha, map_list, NULL);
	
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
	ProtobufCMessage *unpacked_message;
	const gchar *raw_response;
	guchar *decoded_response;
	gsize response_len;
	const gchar *content_type;
	
	if (purple_http_response_get_error(response) != NULL) {
		g_free(request_info);
		g_free(response_message);
		purple_debug_error("hangouts", "Error from server: (%s) %s\n", purple_http_response_get_error(response), purple_http_response_get_data(response, NULL));
		return; //TODO should we send NULL to the callee?
	}
	
	if (callback != NULL) {
		raw_response = purple_http_response_get_data(response, NULL);
		
		content_type = purple_http_response_get_header(response, "X-Goog-Safety-Content-Type");
		if (g_strcmp0(content_type, "application/x-protobuf") == 0) {
			decoded_response = g_base64_decode(raw_response, &response_len);
			unpacked_message = protobuf_c_message_unpack(response_message->descriptor, NULL, response_len, decoded_response);
			
			if (unpacked_message != NULL) {
				if (purple_debug_is_verbose()) {
					gchar *pretty_json = pblite_dump_json(unpacked_message);
					purple_debug_misc("hangouts", "Response: %s", pretty_json);
					g_free(pretty_json);
				}
				
				callback(ha, unpacked_message, real_user_data);
				protobuf_c_message_free_unpacked(unpacked_message, NULL);
			} else {
				purple_debug_error("hangouts", "Error decoding protobuf!\n");
			}
		} else {
			gchar *tidied_json = hangouts_json_tidy_blank_arrays(raw_response);
			JsonArray *response_array = json_decode_array(tidied_json, -1);
			const gchar *first_element = json_array_get_string_element(response_array, 0);
			gboolean ignore_first_element = (first_element != NULL);
			
			pblite_decode(response_message, response_array, ignore_first_element);
			if (ignore_first_element) {
				purple_debug_info("hangouts", "A '%s' says '%s'\n", response_message->descriptor->name, first_element);
			}
			
			if (purple_debug_is_verbose()) {
				gchar *pretty_json = pblite_dump_json(response_message);
				purple_debug_misc("hangouts", "Response: %s", pretty_json);
				g_free(pretty_json);
			}
			
			callback(ha, response_message, real_user_data);
			
			json_array_unref(response_array);
			g_free(tidied_json);
		}
	}
	
	g_free(request_info);
	g_free(response_message);
}

PurpleHttpConnection *
hangouts_client6_request(HangoutsAccount *ha, const gchar *path, HangoutsContentType request_type, const gchar *request_data, gssize request_len, HangoutsContentType response_type, PurpleHttpCallback callback, gpointer user_data)
{
	PurpleHttpRequest *request;
	PurpleHttpConnection *connection;
	const gchar *response_type_str;
	
	switch (response_type) {
		default:
		case HANGOUTS_CONTENT_TYPE_NONE:
		case HANGOUTS_CONTENT_TYPE_JSON:
			response_type_str = "json";
			break;
		case HANGOUTS_CONTENT_TYPE_PBLITE:
			response_type_str = "protojson";
			break;
		case HANGOUTS_CONTENT_TYPE_PROTOBUF:
			response_type_str = "proto";
			break;
	}
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_url_printf(request, HANGOUTS_PBLITE_API_URL "%s%ckey=" GOOGLE_GPLUS_KEY "&alt=%s", path, (strchr(path, '?') ? '&' : '?'), response_type_str);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_keepalive_pool(request, ha->client6_keepalive_pool);
	purple_http_request_set_max_len(request, G_MAXINT32 - 1);
	
	purple_http_request_header_set(request, "X-Goog-Encode-Response-If-Executable", "base64");
	if (request_type != HANGOUTS_CONTENT_TYPE_NONE) {
		purple_http_request_set_method(request, "POST");
		purple_http_request_set_contents(request, request_data, request_len);
		if (request_type == HANGOUTS_CONTENT_TYPE_PROTOBUF) {
			purple_http_request_header_set(request, "Content-Type", "application/x-protobuf");
		} else if (request_type == HANGOUTS_CONTENT_TYPE_PBLITE) {
			purple_http_request_header_set(request, "Content-Type", "application/json+protobuf");
		} else if (request_type == HANGOUTS_CONTENT_TYPE_JSON) {
			purple_http_request_header_set(request, "Content-Type", "application/json");
		}
	}
	
	hangouts_set_auth_headers(ha, request);
	connection = purple_http_request(ha->pc, request, callback, user_data);
	purple_http_request_unref(request);
	
	return connection;
}

void
hangouts_pblite_request(HangoutsAccount *ha, const gchar *endpoint, ProtobufCMessage *request_message, HangoutsPbliteResponseFunc callback, ProtobufCMessage *response_message, gpointer user_data)
{
	gsize request_len;
	gchar *request_data;
	LazyPblistRequestStore *request_info = g_new0(LazyPblistRequestStore, 1);
	
	JsonArray *request_encoded = pblite_encode(request_message);
	JsonNode *node = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(node, request_encoded);
	request_data = json_encode(node, &request_len);
	json_node_free(node);
	
	request_info->ha = ha;
	request_info->callback = callback;
	request_info->response_message = response_message;
	request_info->user_data = user_data;
	
	if (purple_debug_is_verbose()) {
		gchar *pretty_json = pblite_dump_json(request_message);
		purple_debug_misc("hangouts", "Request:  %s", pretty_json);
		g_free(pretty_json);
	}
	
	hangouts_client6_request(ha, endpoint, HANGOUTS_CONTENT_TYPE_PBLITE, request_data, request_len, HANGOUTS_CONTENT_TYPE_PBLITE, hangouts_pblite_request_cb, request_info);
	
	g_free(request_data);
}


void
hangouts_default_response_dump(HangoutsAccount *ha, ProtobufCMessage *response, gpointer user_data)
{
	gchar *dump = pblite_dump_json(response);
	purple_debug_info("hangouts", "%s\n", dump);
	g_free(dump);
}

gboolean
hangouts_set_active_client(PurpleConnection *pc)
{
	HangoutsAccount *ha;
	SetActiveClientRequest request;
	
	switch(purple_connection_get_state(pc)) {
		case PURPLE_CONNECTION_DISCONNECTED:
			// I couldn't eat another bite
			return FALSE;
		case PURPLE_CONNECTION_CONNECTING:
			// Come back for more later
			return TRUE;
		default:
			break;
	}
	
	ha = purple_connection_get_protocol_data(pc);
	if (ha == NULL) {
		g_warn_if_reached();
		return TRUE;
	}
	
	if (ha->active_client_state == ACTIVE_CLIENT_STATE__ACTIVE_CLIENT_STATE_IS_ACTIVE) {
		//We're already the active client
		return TRUE;
	}
	if (ha->idle_time > HANGOUTS_ACTIVE_CLIENT_TIMEOUT) {
		//We've gone idle
		return TRUE;
	}
	if (!purple_presence_is_status_primitive_active(purple_account_get_presence(ha->account), PURPLE_STATUS_AVAILABLE)) {
		//We're marked as not available somehow
		return TRUE;
	}
	ha->active_client_state = ACTIVE_CLIENT_STATE__ACTIVE_CLIENT_STATE_IS_ACTIVE;
	
	set_active_client_request__init(&request);
	
	request.request_header = hangouts_get_request_header(ha);
	request.has_is_active = TRUE;
	request.is_active = TRUE;
	request.full_jid = g_strdup_printf("%s/%s", purple_account_get_username(ha->account), ha->client_id);
	request.has_timeout_secs = TRUE;
	request.timeout_secs = HANGOUTS_ACTIVE_CLIENT_TIMEOUT;
	
	hangouts_pblite_set_active_client(ha, &request, (HangoutsPbliteSetActiveClientResponseFunc)hangouts_default_response_dump, NULL);
	
	hangouts_request_header_free(request.request_header);
	g_free(request.full_jid);
	
	return TRUE;
}


void
hangouts_search_results_send_im(PurpleConnection *pc, GList *row, void *user_data)
{
	PurpleAccount *account = purple_connection_get_account(pc);
	const gchar *who = g_list_nth_data(row, 0);
	PurpleIMConversation *imconv;
	
	imconv = purple_conversations_find_im_with_account(who, account);
	if (imconv == NULL) {
		imconv = purple_im_conversation_new(account, who);
	}
	purple_conversation_present(PURPLE_CONVERSATION(imconv));
}

void
hangouts_search_results_get_info(PurpleConnection *pc, GList *row, void *user_data)
{
	hangouts_get_info(pc, g_list_nth_data(row, 0));
}

void
hangouts_search_results_add_buddy(PurpleConnection *pc, GList *row, void *user_data)
{
	PurpleAccount *account = purple_connection_get_account(pc);

	if (!purple_blist_find_buddy(account, g_list_nth_data(row, 0)))
		purple_blist_request_add_buddy(account, g_list_nth_data(row, 0), "Hangouts", g_list_nth_data(row, 1));
}

void
hangouts_search_users_text_cb(PurpleHttpConnection *connection, PurpleHttpResponse *response, gpointer user_data)
{
	HangoutsAccount *ha = user_data;
	const gchar *response_data;
	size_t response_size;
	JsonArray *resultsarray;
	JsonObject *node;
	gint index, length;
	gchar *search_term;
	JsonObject *status;
	
	PurpleNotifySearchResults *results;
	PurpleNotifySearchColumn *column;
	
	if (purple_http_response_get_error(response) != NULL) {
		purple_notify_error(ha->pc, _("Search Error"), _("There was an error searching for the user"), purple_http_response_get_error(response), purple_request_cpar_from_connection(ha->pc));
		g_dataset_destroy(connection);
		return;
	}
	
	response_data = purple_http_response_get_data(response, &response_size);
	node = json_decode_object(response_data, response_size);
	
	search_term = g_dataset_get_data(connection, "search_term");
	resultsarray = json_object_get_array_member(node, "results");
	length = json_array_get_length(resultsarray);
	
	if (length == 0) {
		status = json_object_get_object_member(node, "status");
		
		if (!json_object_has_member(status, "personalResultsNotReady") || json_object_get_boolean_member(status, "personalResultsNotReady") == TRUE) {
			//Not ready yet, retry
			hangouts_search_users_text(ha, search_term);
			
		} else {		
			gchar *primary_text = g_strdup_printf(_("Your search for the user \"%s\" returned no results"), search_term);
			purple_notify_warning(ha->pc, _("No users found"), primary_text, "", purple_request_cpar_from_connection(ha->pc));
			g_free(primary_text);
		}
		
		g_dataset_destroy(connection);
		json_object_unref(node);
		return;
	}
	
	results = purple_notify_searchresults_new();
	if (results == NULL)
	{
		g_dataset_destroy(connection);
		json_object_unref(node);
		return;
	}
		
	/* columns: Friend ID, Name, Network */
	column = purple_notify_searchresults_column_new(_("ID"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Display Name"));
	purple_notify_searchresults_column_add(results, column);
	
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_ADD, hangouts_search_results_add_buddy);
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_INFO, hangouts_search_results_get_info);
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_IM, hangouts_search_results_send_im);
	
	for(index = 0; index < length; index++)
	{
		JsonNode *result = json_array_get_element(resultsarray, index);
		
		gchar *id = hangouts_json_path_query_string(result, "$.person.personId", NULL);
		gchar *displayname = hangouts_json_path_query_string(result, "$.person.name[*].displayName", NULL);
		GList *row = NULL;
		
		row = g_list_append(row, id);
		row = g_list_append(row, displayname);
		
		purple_notify_searchresults_row_add(results, row);
	}
	
	purple_notify_searchresults(ha->pc, NULL, search_term, NULL, results, NULL, NULL);
	
	g_dataset_destroy(connection);
	json_object_unref(node);
}

/* 

POST https://people-pa.clients6.google.com/v2/people/lookup
id=actual_email_address%40gmail.com&type=EMAIL&matchType=LENIENT&requestMask.includeField.paths=person.email&requestMask.includeField.paths=person.gender&requestMask.includeField.paths=person.in_app_reachability&requestMask.includeField.paths=person.metadata&requestMask.includeField.paths=person.name&requestMask.includeField.paths=person.phone&requestMask.includeField.paths=person.photo&requestMask.includeField.paths=person.read_only_profile_info&extensionSet.extensionNames=HANGOUTS_ADDITIONAL_DATA&extensionSet.extensionNames=HANGOUTS_OFF_NETWORK_GAIA_LOOKUP&extensionSet.extensionNames=HANGOUTS_PHONE_DATA&coreIdParams.useRealtimeNotificationExpandedAcls=true&key=AIzaSyAfFJCeph-euFSwtmqFZi0kaKk-cZ5wufM

id=%2B123456789&type=PHONE&matchType=LENIENT&requestMask.includeField.paths=person.email&requestMask.includeField.paths=person.gender&requestMask.includeField.paths=person.in_app_reachability&requestMask.includeField.paths=person.metadata&requestMask.includeField.paths=person.name&requestMask.includeField.paths=person.phone&requestMask.includeField.paths=person.photo&requestMask.includeField.paths=person.read_only_profile_info&extensionSet.extensionNames=HANGOUTS_ADDITIONAL_DATA&extensionSet.extensionNames=HANGOUTS_OFF_NETWORK_GAIA_LOOKUP&extensionSet.extensionNames=HANGOUTS_PHONE_DATA&coreIdParams.useRealtimeNotificationExpandedAcls=true&quotaFilterType=PHONE&key=AIzaSyAfFJCeph-euFSwtmqFZi0kaKk-cZ5wufM

*/


void
hangouts_search_users_text(HangoutsAccount *ha, const gchar *text)
{
	PurpleHttpRequest *request;
	GString *url = g_string_new("https://people-pa.clients6.google.com/v2/people/autocomplete?");
	PurpleHttpConnection *connection;
	
	g_string_append_printf(url, "query=%s&", purple_url_encode(text));
	g_string_append(url, "client=HANGOUTS_WITH_DATA&");
	g_string_append(url, "pageSize=20&");
	g_string_append_printf(url, "key=%s&", purple_url_encode(GOOGLE_GPLUS_KEY));
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_url(request, url->str);
	
	hangouts_set_auth_headers(ha, request);

	connection = purple_http_request(ha->pc, request, hangouts_search_users_text_cb, ha);
	purple_http_request_unref(request);
	
	g_dataset_set_data_full(connection, "search_term", g_strdup(text), g_free);
	
	g_string_free(url, TRUE);
}

void
hangouts_search_users(PurpleProtocolAction *action)
{
	PurpleConnection *pc = purple_protocol_action_get_connection(action);
	HangoutsAccount *ha = purple_connection_get_protocol_data(pc);
	
	purple_request_input(pc, _("Search for friends..."),
					   _("Search for friends..."),
					   NULL,
					   NULL, FALSE, FALSE, NULL,
					   _("_Search"), G_CALLBACK(hangouts_search_users_text),
					   _("_Cancel"), NULL,
					   purple_request_cpar_from_connection(pc),
					   ha);

}
