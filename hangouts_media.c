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

#include "hangouts_media.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "hangouts_pblite.h"
#include "hangouts_json.h"
#include "hangout_media.pb-c.h"
#include "hangouts_connection.h"
#include "hangouts_conversation.h" //just for hangouts_get_request_header()

#include "debug.h"
#include "mediamanager.h"
#include "media.h"


#if defined(_WIN32) && !PURPLE_VERSION_CHECK(3, 0, 0)
/** This is a bit of a hack; 
		if libpurple isn't compiled with USE_VV then a bunch of functions don't end up being exported, so we'll try load them in at runtime so we dont have to have a million and one different versions of the .so
*/
static gboolean purple_media_functions_initaliased = FALSE;
static gpointer libpurple_module;
// media/candidates.h
static gchar *(*_purple_media_candidate_get_username)(PurpleMediaCandidate *candidate);
static gchar *(*_purple_media_candidate_get_password)(PurpleMediaCandidate *candidate);
static PurpleMediaNetworkProtocol (*_purple_media_candidate_get_protocol)(PurpleMediaCandidate *candidate);
static guint (*_purple_media_candidate_get_component_id)(PurpleMediaCandidate *candidate);
static gchar *(*_purple_media_candidate_get_ip)(PurpleMediaCandidate *candidate);
static guint16 (*_purple_media_candidate_get_port)(PurpleMediaCandidate *candidate);
static PurpleMediaCandidateType (*_purple_media_candidate_get_candidate_type)(PurpleMediaCandidate *candidate);
static guint32 (*_purple_media_candidate_get_priority)(PurpleMediaCandidate *candidate);
static PurpleMediaCandidate *(*_purple_media_candidate_new)(const gchar *foundation, guint component_id, PurpleMediaCandidateType type, PurpleMediaNetworkProtocol proto, const gchar *ip, guint port);
// media/codecs.h
static guint (*_purple_media_codec_get_id)(PurpleMediaCodec *codec);
static gchar *(*_purple_media_codec_get_encoding_name)(PurpleMediaCodec *codec);
static guint (*_purple_media_codec_get_clock_rate)(PurpleMediaCodec *codec);
static guint (*_purple_media_codec_get_channels)(PurpleMediaCodec *codec);
static GList *(*_purple_media_codec_get_optional_parameters)(PurpleMediaCodec *codec);
static PurpleMediaCodec *(*_purple_media_codec_new)(int id, const char *encoding_name, PurpleMediaSessionType media_type, guint clock_rate);
static void (*_purple_media_codec_add_optional_parameter)(PurpleMediaCodec *codec, const gchar *name, const gchar *value);
// media/backend-fs2.h
static GType (*_purple_media_backend_fs2_get_type)(void);

// Using dlopen() instead of GModule because I don't want another dep
#ifdef _WIN32
#	include <windows.h>
#	define dlopen(filename, flag)  GetModuleHandleA(filename)
#	define dlsym(handle, symbol)   GetProcAddress(handle, symbol)
#	define dlclose(handle)         FreeLibrary(handle)
#	define RTLD_LAZY               0x0001
#else
#	include <dlfcn.h>
#endif

static void
hangouts_init_media_functions()
{	
	if (purple_media_functions_initaliased == FALSE) {
		libpurple_module = dlopen(NULL, RTLD_LAZY);
		if (libpurple_module != NULL) {
			// media/candidates.h
			_purple_media_candidate_get_username = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_username");
			_purple_media_candidate_get_password = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_password");
			_purple_media_candidate_get_component_id = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_component_id");
			_purple_media_candidate_get_protocol = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_protocol");
			_purple_media_candidate_get_ip = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_ip");
			_purple_media_candidate_get_port = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_port");
			_purple_media_candidate_get_candidate_type = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_candidate_type");
			_purple_media_candidate_get_priority = (gpointer) dlsym(libpurple_module, "purple_media_candidate_get_priority");
			_purple_media_candidate_new = (gpointer) dlsym(libpurple_module, "purple_media_candidate_new");
			
			// media/codecs.h
			_purple_media_codec_get_id = (gpointer) dlsym(libpurple_module, "purple_media_codec_get_id");
			_purple_media_codec_get_encoding_name = (gpointer) dlsym(libpurple_module, "purple_media_codec_get_encoding_name");
			_purple_media_codec_get_clock_rate = (gpointer) dlsym(libpurple_module, "purple_media_codec_get_clock_rate");
			_purple_media_codec_get_channels = (gpointer) dlsym(libpurple_module, "purple_media_codec_get_channels");
			_purple_media_codec_get_optional_parameters = (gpointer) dlsym(libpurple_module, "purple_media_codec_get_optional_parameters");
			_purple_media_codec_new = (gpointer) dlsym(libpurple_module, "purple_media_codec_new");
			_purple_media_codec_add_optional_parameter = (gpointer) dlsym(libpurple_module, "purple_media_codec_add_optional_parameter");
			
			// media/backend-fs2.h
			_purple_media_backend_fs2_get_type = (gpointer) dlsym(libpurple_module, "purple_media_backend_fs2_get_type");
			
			purple_media_functions_initaliased = TRUE;
		}
	}
}
#else /*PURPLE_VERSION_CHECK*/

#define _purple_media_candidate_get_username         purple_media_candidate_get_username
#define _purple_media_candidate_get_password         purple_media_candidate_get_password 
#define _purple_media_candidate_get_protocol         purple_media_candidate_get_protocol 
#define _purple_media_candidate_get_component_id     purple_media_candidate_get_component_id 
#define _purple_media_candidate_get_ip               purple_media_candidate_get_ip 
#define _purple_media_candidate_get_port             purple_media_candidate_get_port 
#define _purple_media_candidate_get_candidate_type   purple_media_candidate_get_candidate_type 
#define _purple_media_candidate_get_priority         purple_media_candidate_get_priority 
#define _purple_media_candidate_new                  purple_media_candidate_new 
#define _purple_media_codec_get_id                   purple_media_codec_get_id 
#define _purple_media_codec_get_encoding_name        purple_media_codec_get_encoding_name 
#define _purple_media_codec_get_clock_rate           purple_media_codec_get_clock_rate 
#define _purple_media_codec_get_channels             purple_media_codec_get_channels 
#define _purple_media_codec_get_optional_parameters  purple_media_codec_get_optional_parameters
#define _purple_media_codec_new                      purple_media_codec_new
#define _purple_media_codec_add_optional_parameter   purple_media_codec_add_optional_parameter
#define _purple_media_backend_fs2_get_type           purple_media_backend_fs2_get_type

static void
hangouts_init_media_functions()
{
	
}

#endif /*PURPLE_VERSION_CHECK*/



// This is a hack that lets us get at some private variables we need from farsight
typedef struct _PurpleMediaBackendFs2Session
{
	gpointer backend;
	gchar *id;
	GObject *session;
} PurpleMediaBackendFs2Session;

typedef struct _PurpleMediaBackendFs2Private
{
	PurpleMedia *media;
	gpointer confbin;
	gpointer conference;
	gchar *conference_type;

	gpointer potential_sessions_1;
	gpointer potential_sessions_2;
} PurpleMediaBackendFs2Private;


GList *
purple_media_get_session_ssrcs(PurpleMedia *media, const gchar *session_id)
{
	PurpleMediaBackendFs2Private *priv;
	PurpleMediaBackendFs2Session *session;
	GList *ssrcs = NULL;
	GObject *purple_media_backend;
	GHashTable *sessions;
	
	g_return_val_if_fail(PURPLE_IS_MEDIA(media), NULL);
	
	g_object_get(media, "backend", &purple_media_backend, NULL);
	priv = G_TYPE_INSTANCE_GET_PRIVATE((purple_media_backend), G_OBJECT_TYPE(purple_media_backend), PurpleMediaBackendFs2Private);
	
	// could be at either of these two points in the struct, depending on #define options
	if (G_IS_OBJECT(priv->potential_sessions_1)) {
		sessions = (GHashTable *) priv->potential_sessions_2;
	} else {
		sessions = (GHashTable *) priv->potential_sessions_1;
	}
	
	if (sessions != NULL) {
		guint ssrc;
		
		session = g_hash_table_lookup(sessions, session_id);
		if (session != NULL) {
			g_object_get(session->session, "ssrc", &ssrc, NULL);
			if (ssrc == 0) {
				ssrc = g_random_int();
				g_object_set(session->session, "ssrc", ssrc, NULL);
			}
			
			ssrcs = g_list_append(ssrcs, GINT_TO_POINTER(ssrc));
		}
	}
	
	return ssrcs;
}
// end giant hack


typedef struct {
	HangoutsAccount *ha;
	gchar *hangout_id;
	PurpleMedia *media;
	gchar *who;
	PurpleMediaSessionType type;
	guchar *encryption_key;
	guchar *decryption_key;
	gchar *hangout_cookie;
	gchar *participant_id;
	gchar *session_id;
} HangoutsMedia;

static void hangouts_media_send_media_stream_add(HangoutsAccount *ha, HangoutsMedia *hangouts_media);

static void
hangouts_media_destroy(HangoutsMedia *hangouts_media)
{
	purple_media_set_protocol_data(hangouts_media->media, NULL);
	
	g_free(hangouts_media->session_id);
	g_free(hangouts_media->participant_id);
	g_free(hangouts_media->encryption_key);
	g_free(hangouts_media->decryption_key);
	g_free(hangouts_media->hangout_cookie);
	g_free(hangouts_media->hangout_id);
	g_free(hangouts_media->who);
	g_free(hangouts_media);
}

static MediaType
hangout_get_session_media_type(PurpleMedia *media, gchar *sid) {
	PurpleMediaSessionType purple_session_type = purple_media_get_session_type(media, sid);
	
	if (purple_session_type & PURPLE_MEDIA_AUDIO) {
		if (purple_session_type & PURPLE_MEDIA_VIDEO) {
			return MEDIA_TYPE__MEDIA_TYPE_BUNDLE;
		} else {
			return MEDIA_TYPE__MEDIA_TYPE_AUDIO;
		}
	} else if (purple_session_type & PURPLE_MEDIA_VIDEO) {
		return MEDIA_TYPE__MEDIA_TYPE_VIDEO;
#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
	} else if (purple_session_type & PURPLE_MEDIA_APPLICATION) {
		return MEDIA_TYPE__MEDIA_TYPE_DATA;
#endif
	} else {
		return 0;
	}
}

static void
hangouts_pblite_media_media_session_add_cb(HangoutsAccount *ha, MediaSessionAddResponse *response, gpointer user_data)
{
	HangoutsMedia *hangouts_media = user_data;
	guint i, j, k, l;
	
	purple_debug_info("hangouts", "hangouts_pblite_media_media_session_add_cb: ");
	hangouts_default_response_dump(ha, &response->base, user_data);
	
	for (i = 0; i < response->n_resource; i++) {
		MediaSession *resource = response->resource[i];
		hangouts_media->session_id = g_strdup(resource->session_id);
		for (j = 0; j < resource->n_server_content; j++) {
			MediaContent *server_content = resource->server_content[j];
			GList *remote_candidates_list = NULL;
			GList *remote_codecs_list = NULL;
			
			for (k = 0; k < server_content->transport->n_candidate; k++) {
				MediaIceCandidate *candidate = server_content->transport->candidate[k];
				PurpleMediaCandidate *purple_candidate;
				guint component_id;
				PurpleMediaCandidateType candidate_type;
				PurpleMediaNetworkProtocol network_protocol;
				
				switch(candidate->component) {
					case COMPONENT__RTP:
						component_id = PURPLE_MEDIA_COMPONENT_RTP;
						break;
					case COMPONENT__RTCP:
						component_id = PURPLE_MEDIA_COMPONENT_RTCP;
						break;
					default:
						continue;
				}
				
				switch(candidate->type) {
					case MEDIA_ICE_CANDIDATE_TYPE__HOST:
						candidate_type = PURPLE_MEDIA_CANDIDATE_TYPE_HOST;
						break;
					case MEDIA_ICE_CANDIDATE_TYPE__SERVER_REFLEXIVE:
						candidate_type = PURPLE_MEDIA_CANDIDATE_TYPE_SRFLX;
						break;
					case MEDIA_ICE_CANDIDATE_TYPE__PEER_REFLEXIVE:
						candidate_type = PURPLE_MEDIA_CANDIDATE_TYPE_PRFLX;
						break;
					case MEDIA_ICE_CANDIDATE_TYPE__RELAY:
						candidate_type = PURPLE_MEDIA_CANDIDATE_TYPE_RELAY;
						break;
					default:
						continue;
				}
				
				switch(candidate->protocol) {
					case PROTOCOL__UDP:
						network_protocol = PURPLE_MEDIA_NETWORK_PROTOCOL_UDP;
						break;
					case PROTOCOL__TCP:
					case PROTOCOL__SSLTCP:
#if PURPLE_VERSION_CHECK(3, 0, 0)
						network_protocol = PURPLE_MEDIA_NETWORK_PROTOCOL_TCP_ACTIVE;
#else
						network_protocol = PURPLE_MEDIA_NETWORK_PROTOCOL_TCP;
#endif
						break;
					default:
						continue;
				}

				purple_candidate = _purple_media_candidate_new(
					"", component_id, candidate_type,
					network_protocol, candidate->ip, candidate->port);
				g_object_set(purple_candidate,
						"username", server_content->transport->username,
						"password", server_content->transport->password,
						"priority", candidate->priority, NULL);
				
				remote_candidates_list = g_list_append(remote_candidates_list, purple_candidate);
			}
			purple_media_add_remote_candidates(hangouts_media->media, "hangout", hangouts_media->who, remote_candidates_list);
			
			for (k = 0; k < server_content->n_codec; k++) {
				MediaCodec *codec = server_content->codec[k];
				PurpleMediaCodec *purple_codec;
				PurpleMediaSessionType type;
				
				switch(codec->media_type) {
					case MEDIA_TYPE__MEDIA_TYPE_VIDEO:
						type = PURPLE_MEDIA_VIDEO;
						break;
					case MEDIA_TYPE__MEDIA_TYPE_AUDIO:
						type = PURPLE_MEDIA_AUDIO;
						break;
					case MEDIA_TYPE__MEDIA_TYPE_BUNDLE:
						type = PURPLE_MEDIA_VIDEO | PURPLE_MEDIA_AUDIO;
						break;
#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
					case MEDIA_TYPE__MEDIA_TYPE_DATA:
						type = PURPLE_MEDIA_APPLICATION;
						break;
#endif
					default:
						continue;
				}
				purple_codec = _purple_media_codec_new(
					codec->payload_id, codec->name,
					type, codec->sample_rate);

				for (l = 0; l < codec->n_param; l++) {
					MediaCodecParam *param = codec->param[l];
					_purple_media_codec_add_optional_parameter(purple_codec,
							param->key, param->value);
				}
				g_object_set(purple_codec,
						"channels", codec->channel_count, NULL);
				
				remote_codecs_list = g_list_append(remote_codecs_list, purple_codec);
			}
			purple_media_set_remote_codecs(hangouts_media->media, "hangout", hangouts_media->who, remote_codecs_list);
			
#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
			if (server_content->n_crypto_param > 0) {
				MediaCryptoParams *crypto_param = server_content->crypto_param[0];
				const gchar *crypto_algo;
				gchar *key_encoded;
				gsize key_len;
				
				switch(crypto_param->suite) {
					case MEDIA_CRYPTO_SUITE__AES_CM_128_HMAC_SHA1_80:
					default:
						crypto_algo = "hmac-sha1-80";
						break;
					case MEDIA_CRYPTO_SUITE__AES_CM_128_HMAC_SHA1_32:
						crypto_algo = "hmac-sha1-32";
						break;
				}
				
				key_encoded = crypto_param->key_params;
				if (g_str_has_prefix(key_encoded, "inline:")) {
					key_encoded += 7;
				}
				hangouts_media->decryption_key = g_base64_decode(key_encoded, &key_len);
				
				purple_media_set_decryption_parameters(hangouts_media->media,
						"hangout", hangouts_media->who, "aes-128-icm", crypto_algo,
						(gchar *)hangouts_media->decryption_key, key_len);
			}
#endif
		}
	}

	// TODO: Find a better place for this?
	purple_media_stream_info(hangouts_media->media,
			PURPLE_MEDIA_INFO_ACCEPT, NULL, NULL, FALSE);
	
	if (hangouts_media->participant_id != NULL) {		
		hangouts_media_send_media_stream_add(ha, hangouts_media);
	}
}

static void
hangouts_send_media_and_codecs(PurpleMedia *media, gchar *sid, gchar *name, HangoutsMedia *hangouts_media)
{
	MediaSessionAddRequest request;
	MediaSession media_session;
	MediaSession *media_sessions;
	MediaContent client_content;
	MediaContent *client_contents;
	MediaTransport transport;
	MediaIceCandidate **ice_candidates;
	guint n_ice_candidates;
	GList *purple_candidates;
#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
	MediaCryptoParams crypto_param;
	MediaCryptoParams *crypto_params;
	gchar *hangouts_crypto_base64, *hangouts_crypto_key;
#endif
	guint i, j;
	
	GList *purple_codecs;
	MediaCodec **codecs;
	guint n_codecs;
	PurpleMediaSessionType purple_media_type;
	GList *purple_codec_params;
	MediaCodecParam **params;
	guint n_params;

	if (purple_media_accepted(media, NULL, NULL)) {
		purple_debug_info("hangouts",
				"Don't send session add request again.");
		return;
	}
	
	media_session_add_request__init(&request);
	media_session__init(&media_session);
	media_content__init(&client_content);
	media_transport__init(&transport);
	
	// Prefer RFC ICE
	transport.has_ice_version = TRUE;
	transport.ice_version = ICE_VERSION__ICE_RFC_5245;
	
	purple_candidates = purple_media_get_local_candidates(media, sid, name);
	n_ice_candidates = g_list_length(purple_candidates);
	ice_candidates = g_new0(MediaIceCandidate *, n_ice_candidates);
	for(i = 0; purple_candidates; purple_candidates = g_list_next(purple_candidates), i++) {
		PurpleMediaCandidate *purple_candidate = purple_candidates->data;
		MediaIceCandidate *ice_candidate = ice_candidates[i] = g_new0(MediaIceCandidate, 1);
		media_ice_candidate__init(ice_candidate);
		
		//TODO multiple passwords needed?
		transport.username = _purple_media_candidate_get_username(purple_candidate);
		transport.password = _purple_media_candidate_get_password(purple_candidate);
		
		ice_candidate->has_component = TRUE;
		switch(_purple_media_candidate_get_component_id(purple_candidate)) {
			case PURPLE_MEDIA_COMPONENT_RTP:
				ice_candidate->component = COMPONENT__RTP;
				break;
			case PURPLE_MEDIA_COMPONENT_RTCP:
				ice_candidate->component = COMPONENT__RTCP;
				break;
			default:
				ice_candidate->has_component = FALSE;
				break;
		}
		
		ice_candidate->has_protocol = TRUE;
		switch(_purple_media_candidate_get_protocol(purple_candidate)) {
			case PURPLE_MEDIA_NETWORK_PROTOCOL_UDP:
				ice_candidate->protocol = PROTOCOL__UDP;
				break;
#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
			case PURPLE_MEDIA_NETWORK_PROTOCOL_TCP_PASSIVE:
			case PURPLE_MEDIA_NETWORK_PROTOCOL_TCP_ACTIVE:
			case PURPLE_MEDIA_NETWORK_PROTOCOL_TCP_SO:
#else
			case PURPLE_MEDIA_NETWORK_PROTOCOL_TCP:
#endif
				ice_candidate->protocol = PROTOCOL__TCP;
				break;
			default:
				ice_candidate->has_protocol = FALSE;
				break;
		}
		
		ice_candidate->ip = _purple_media_candidate_get_ip(purple_candidate);
		ice_candidate->has_port = TRUE;
		ice_candidate->port = _purple_media_candidate_get_port(purple_candidate);
		
		ice_candidate->has_type = TRUE;
		switch(_purple_media_candidate_get_candidate_type(purple_candidate)) {
			case PURPLE_MEDIA_CANDIDATE_TYPE_HOST:
				ice_candidate->type = MEDIA_ICE_CANDIDATE_TYPE__HOST;
				break;
			case PURPLE_MEDIA_CANDIDATE_TYPE_SRFLX:
				ice_candidate->type = MEDIA_ICE_CANDIDATE_TYPE__SERVER_REFLEXIVE;
				break;
			case PURPLE_MEDIA_CANDIDATE_TYPE_PRFLX:
				ice_candidate->type = MEDIA_ICE_CANDIDATE_TYPE__PEER_REFLEXIVE;
				break;
			case PURPLE_MEDIA_CANDIDATE_TYPE_RELAY:
				ice_candidate->type = MEDIA_ICE_CANDIDATE_TYPE__RELAY;
				break;
			default:
			case PURPLE_MEDIA_CANDIDATE_TYPE_MULTICAST:
				ice_candidate->has_type = FALSE;
				break;
		}
		
		ice_candidate->priority = _purple_media_candidate_get_priority(purple_candidate);
		ice_candidate->has_priority = TRUE;
	}
	transport.candidate = ice_candidates;
	transport.n_candidate = n_ice_candidates;
	
	client_content.transport = &transport;
	
	// -- CODECS --
	purple_codecs = purple_media_get_codecs(media, sid);
	n_codecs = g_list_length(purple_codecs);
	codecs = g_new0(MediaCodec *, n_codecs);
	for(i = 0; purple_codecs; purple_codecs = g_list_next(purple_codecs), i++) {
		PurpleMediaCodec *purple_codec = purple_codecs->data;
		MediaCodec *codec = codecs[i] = g_new0(MediaCodec, 1);
		media_codec__init(codec);
		
		codec->has_payload_id = TRUE;
		codec->payload_id = _purple_media_codec_get_id(purple_codec);
		
		codec->name = _purple_media_codec_get_encoding_name(purple_codec);
		
		g_object_get(purple_codec, "media-type", &purple_media_type, NULL);
		codec->has_media_type = TRUE;
		if (purple_media_type & PURPLE_MEDIA_VIDEO) {
			codec->media_type = MEDIA_TYPE__MEDIA_TYPE_VIDEO;
		} else if (purple_media_type & PURPLE_MEDIA_AUDIO) {
			codec->media_type = MEDIA_TYPE__MEDIA_TYPE_AUDIO;
		} else {
			codec->has_media_type = FALSE;
		}
		
		//codec.preference = ; not set
		
		codec->has_sample_rate = TRUE;
		codec->sample_rate = _purple_media_codec_get_clock_rate(purple_codec);
		
		if (g_strcmp0(codec->name, "PCMA") == 0 ||
			g_strcmp0(codec->name, "PCMU") == 0) {
			//TODO is this needed?
			codec->has_bit_rate = TRUE;
			codec->bit_rate = 48000;
		}
		
		codec->has_channel_count = TRUE;
		codec->channel_count = _purple_media_codec_get_channels(purple_codec);
		
		purple_codec_params = _purple_media_codec_get_optional_parameters(purple_codec);
		n_params = g_list_length(purple_codec_params);
		params = g_new0(MediaCodecParam *, n_params);
		for(j = 0; purple_codec_params; purple_codec_params = g_list_next(purple_codec_params), j++) {
			PurpleKeyValuePair *param_info = purple_codec_params->data;
			MediaCodecParam *param;
			
			if (g_strcmp0(param_info->key, "bitrate") == 0) {
				codec->has_bit_rate = TRUE;
				codec->bit_rate = atoi(param_info->value);
				n_params--;
				j--;
				continue;
			}
			
			param = params[j] = g_new0(MediaCodecParam, 1);
			media_codec_param__init(param);
			
			param->key = param_info->key;
			param->value = param_info->value;
		}
		codec->n_param = n_params;
		codec->param = params;
	}
	
	client_content.n_codec = n_codecs;
	client_content.codec = codecs;
	
#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
	//Generate a SRTP key
	g_free(hangouts_media->encryption_key);
	hangouts_media->encryption_key = g_new0(guchar, SRTP_KEY_LEN);
	for (i = 0; i < SRTP_KEY_LEN; i++) {
		hangouts_media->encryption_key[i] = rand() & 0xff;
	}
	
	media_crypto_params__init(&crypto_param);
	crypto_param.has_suite = TRUE;
	crypto_param.suite = MEDIA_CRYPTO_SUITE__AES_CM_128_HMAC_SHA1_80;
	
	hangouts_crypto_base64 = g_base64_encode(hangouts_media->encryption_key, SRTP_KEY_LEN);
	hangouts_crypto_key = g_strconcat("inline:", hangouts_crypto_base64, NULL);
	crypto_param.key_params = hangouts_crypto_key;
	
	crypto_param.has_tag = TRUE;
	crypto_param.tag = 1;
	
	crypto_params = &crypto_param;
	client_content.n_crypto_param = 1;
	client_content.crypto_param = &crypto_params;
	
	purple_media_set_encryption_parameters(media,
			sid, "aes-128-icm", "hmac-sha1-80",
			(gchar *)hangouts_media->encryption_key, SRTP_KEY_LEN);
#endif
	
	client_contents = &client_content;
	media_session.n_client_content = 1;
	media_session.client_content = &client_contents;
	
	client_content.has_media_type = TRUE;
	client_content.media_type = hangout_get_session_media_type(media, sid);
	
	media_sessions = &media_session;
	request.n_resource = 1;
	request.resource = &media_sessions;
	request.request_header = hangouts_get_request_header(hangouts_media->ha);
	
	purple_debug_info("hangouts", "hangouts_pblite_media_media_session_add: ");
	hangouts_default_response_dump(NULL, (ProtobufCMessage*)&request, NULL);	
	hangouts_pblite_media_media_session_add(hangouts_media->ha, &request, hangouts_pblite_media_media_session_add_cb, hangouts_media);
	
	hangouts_request_header_free(request.request_header);
	for(i = 0; i < n_ice_candidates; i++) {
		g_free(ice_candidates[i]);
	}
	g_free(ice_candidates);
	
	for(i = 0; i < n_codecs; i++) {
		for(j = 0; j < codecs[i]->n_param; j++) {
			g_free(codecs[i]->param[j]);
		}
		g_free(codecs[i]->param);
		g_free(codecs[i]);
	}
	g_free(codecs);
	
#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
	g_free(hangouts_crypto_base64);
	g_free(hangouts_crypto_key);
#endif
}

static void
hangouts_media_codecs_changed_cb(PurpleMedia *media, gchar *sid, HangoutsMedia *hangouts_media)
{
	gchar *name;
	if (!purple_media_codecs_ready(media, sid)) {
		return;
	}
	
	name = hangouts_media->who;
	if (!purple_media_candidates_prepared(media, sid, name)) {
		return;
	}
	
	hangouts_send_media_and_codecs(media, sid, name, hangouts_media);
}


static void
hangouts_media_candidates_prepared_cb(PurpleMedia *media, gchar *sid, gchar *name, HangoutsMedia *hangouts_media)
{
	if (!purple_media_candidates_prepared(media, sid, name)) {
		return;
	}
	
	if (!purple_media_codecs_ready(media, sid)) {
		return;
	}
	
	hangouts_send_media_and_codecs(media, sid, name, hangouts_media);
}

static void
hangouts_media_state_changed_cb(PurpleMedia *media, PurpleMediaState state, gchar *sid, gchar *name, HangoutsMedia *hangouts_media)
{
	HangoutsAccount *ha = hangouts_media->ha;
	
	switch (state) {
		case PURPLE_MEDIA_STATE_END: {
			HangoutParticipantRemoveRequest request;
			
			hangout_participant_remove_request__init(&request);
			
			request.hangout_id = hangouts_media->hangout_id;
			request.request_header = hangouts_get_request_header(ha);
			
			hangouts_pblite_media_hangout_participant_remove(ha, &request, NULL, NULL);
			
			hangouts_request_header_free(request.request_header);
		} break;
		default:
			break;
	}
}

static void
hangout_participant_add_cb(HangoutsAccount *ha, HangoutParticipantAddResponse *response, gpointer user_data)
{
	HangoutsMedia *hangouts_media = user_data;
	
	if (response->sync_metadata && response->sync_metadata->hangout_cookie) {
		hangouts_media->hangout_cookie = g_strdup(response->sync_metadata->hangout_cookie->cookie);
	}
	hangouts_media->participant_id = g_strdup(response->resource[0]->participant_id);
	
	//Add remote to hangout
	{
		HangoutInvitationAddRequest invitation_request;
		HangoutInvitation invitation;
		HangoutInvitee invitee, *invitee_ptr = &invitee;
		HangoutSharingTargetId sharing_target_id;
		PersonId person_id;
		
		hangout_invitation_add_request__init(&invitation_request);
		hangout_invitation__init(&invitation);
		person_id__init(&person_id);
		hangout_sharing_target_id__init(&sharing_target_id);
		hangout_invitee__init(&invitee);
		
		invitation.hangout_id = hangouts_media->hangout_id;
		person_id.user_id = hangouts_media->who;
		sharing_target_id.person_id = &person_id;
		invitee.invitee = &sharing_target_id;
		invitation.n_invited_entity = 1;
		invitation.invited_entity = &invitee_ptr;
		invitation_request.invitation = &invitation;
		invitation_request.request_header = hangouts_get_request_header(ha);
		
		purple_debug_info("hangouts", "hangouts_pblite_media_hangout_invitation_add: ");
		hangouts_default_response_dump(NULL, (ProtobufCMessage*)&invitation_request, NULL);	
		hangouts_pblite_media_hangout_invitation_add(ha, &invitation_request, (HangoutsPbliteHangoutInvitationAddResponseFunc)hangouts_default_response_dump, NULL);
		
		hangouts_request_header_free(invitation_request.request_header);
	}
	
	//Enable media source
	{
		MediaSourceAddRequest source_request;
		size_t n_resource = 0;
		MediaSource audio_media_source;
		MediaSource video_media_source;
		MuteState audio_mute_state;
		MuteState video_mute_state;
		VideoDetails video_details;
		
		media_source_add_request__init(&source_request);
		source_request.request_header = hangouts_get_request_header(ha);
		source_request.resource = g_new0(MediaSource *, 2);
		
		if (hangouts_media->type & PURPLE_MEDIA_AUDIO) {
			media_source__init(&audio_media_source);
			
			audio_media_source.hangout_id = hangouts_media->hangout_id;
			audio_media_source.participant_id = hangouts_media->participant_id;
			audio_media_source.source_id = "1"; //TODO
			audio_media_source.has_media_type = TRUE;
			audio_media_source.media_type = MEDIA_TYPE__MEDIA_TYPE_AUDIO;
			
			mute_state__init(&audio_mute_state);
			audio_mute_state.muted = FALSE;
			audio_media_source.mute_state = &audio_mute_state;
			
			source_request.resource[n_resource++] = &audio_media_source;
		}
		if (hangouts_media->type & PURPLE_MEDIA_VIDEO) {
			media_source__init(&video_media_source);
			
			video_media_source.hangout_id = hangouts_media->hangout_id;
			video_media_source.participant_id = hangouts_media->participant_id;
			video_media_source.source_id = "2"; //TODO
			video_media_source.has_media_type = TRUE;
			video_media_source.media_type = MEDIA_TYPE__MEDIA_TYPE_VIDEO;
			
			mute_state__init(&video_mute_state);
			video_mute_state.muted = FALSE;
			video_media_source.mute_state = &video_mute_state;
			
			video_details__init(&video_details);
			video_details.has_capture_type = TRUE;
			video_details.capture_type = CAPTURE_TYPE__CAMERA; // could be a screencast
			video_media_source.video_details = &video_details;
			
			source_request.resource[n_resource++] = &video_media_source;
		}
		
		source_request.n_resource = n_resource;
		
		purple_debug_info("hangouts", "hangouts_pblite_media_media_source_add: ");
		hangouts_default_response_dump(NULL, (ProtobufCMessage*)&source_request, NULL);	
		hangouts_pblite_media_media_source_add(ha, &source_request, (HangoutsPbliteMediaSourceAddResponseFunc)hangouts_default_response_dump, NULL);
		
		g_free(source_request.resource);
		hangouts_request_header_free(source_request.request_header);
	}
	
	if (hangouts_media->session_id != NULL) {		
		hangouts_media_send_media_stream_add(ha, hangouts_media);
	}
}

static void
hangouts_media_send_media_stream_add(HangoutsAccount *ha, HangoutsMedia *hangouts_media)
{
	// Send ssrc's
	{
		MediaStreamAddRequest stream_request;
		size_t n_resource = 0;
		MediaStream audio_media_stream;
		MediaStream video_media_stream;
		// Male otters are called dogs or boars, females are called bitches or sows, and their offspring are called pups.
		MediaStreamOffer audio_stream_otter;
		MediaStreamOffer video_stream_otter;
		SsrcGroup sim_group;
		SsrcGroup fid_group;
		
		media_stream_add_request__init(&stream_request);
		stream_request.request_header = hangouts_get_request_header(ha);
		stream_request.resource = g_new0(MediaStream *, 2);
		
		if (hangouts_media->type & PURPLE_MEDIA_AUDIO) {
			GList *ssrcs;
			media_stream__init(&audio_media_stream);
			media_stream_offer__init(&audio_stream_otter);
			
			audio_media_stream.has_direction = TRUE;
			audio_media_stream.direction = MEDIA_STREAM_DIRECTION__MEDIA_STREAM_DIRECTION_UP;
			audio_media_stream.has_media_type = TRUE;
			audio_media_stream.media_type = MEDIA_TYPE__MEDIA_TYPE_AUDIO;
			audio_media_stream.session_id = hangouts_media->session_id;
			audio_media_stream.stream_id = "dogboarsowpup/1"; //TODO
			audio_media_stream.hangout_id = hangouts_media->hangout_id;
			audio_media_stream.participant_id = hangouts_media->participant_id;
			audio_media_stream.source_id = "1"; //TODO
			audio_media_stream.offer = &audio_stream_otter;
			
			ssrcs = purple_media_get_session_ssrcs(hangouts_media->media, "hangout");
			//purple_debug_warning("TEST", "ssrcs: %d", g_list_length(ssrcs));
			if (ssrcs != NULL) {
				audio_stream_otter.ssrc = g_new0(uint32_t, g_list_length(ssrcs));
				for(; ssrcs; ssrcs = g_list_delete_link(ssrcs, ssrcs)) {
					audio_stream_otter.ssrc[audio_stream_otter.n_ssrc++] = GPOINTER_TO_INT(ssrcs->data);
				}
			}

			stream_request.resource[n_resource++] = &audio_media_stream;
		}
		
		if (hangouts_media->type & PURPLE_MEDIA_VIDEO) {
			GList *ssrcs;
			media_stream__init(&video_media_stream);
			media_stream_offer__init(&video_stream_otter);
			
			video_media_stream.has_direction = TRUE;
			video_media_stream.direction = MEDIA_STREAM_DIRECTION__MEDIA_STREAM_DIRECTION_UP;
			video_media_stream.has_media_type = TRUE;
			video_media_stream.media_type = MEDIA_TYPE__MEDIA_TYPE_VIDEO;
			video_media_stream.session_id = hangouts_media->session_id;
			video_media_stream.stream_id = "dogboarsowpup/2"; //TODO
			video_media_stream.hangout_id = hangouts_media->hangout_id;
			video_media_stream.participant_id = hangouts_media->participant_id;
			video_media_stream.source_id = "2"; //TODO
			video_media_stream.offer = &video_stream_otter;
			
			ssrcs = purple_media_get_session_ssrcs(hangouts_media->media, "hangoutv");
			if (ssrcs != NULL) {
				//TODO work out what these actually should be
				ssrc_group__init(&sim_group);
				ssrc_group__init(&fid_group);
				sim_group.semantics = "SIM";
				fid_group.semantics = "FID";
				
				video_stream_otter.ssrc = g_new0(uint32_t, g_list_length(ssrcs));
				sim_group.ssrc = g_new0(uint32_t, g_list_length(ssrcs));
				fid_group.ssrc = g_new0(uint32_t, g_list_length(ssrcs));
				
				for(; ssrcs; ssrcs = g_list_delete_link(ssrcs, ssrcs)) {
					video_stream_otter.ssrc[video_stream_otter.n_ssrc++] = GPOINTER_TO_INT(ssrcs->data);
					sim_group.ssrc[sim_group.n_ssrc++] = GPOINTER_TO_INT(ssrcs->data);
					fid_group.ssrc[fid_group.n_ssrc++] = GPOINTER_TO_INT(ssrcs->data);
				}
				
				video_stream_otter.ssrc_group = g_new0(SsrcGroup *, 2);
				video_stream_otter.n_ssrc_group = 2;
				video_stream_otter.ssrc_group[0] = &sim_group;
				video_stream_otter.ssrc_group[1] = &fid_group;
			}
			
			stream_request.resource[n_resource++] = &video_media_stream;
		}
		
		stream_request.n_resource = n_resource;
		
		purple_debug_info("hangouts", "hangouts_pblite_media_media_stream_add: ");
		hangouts_default_response_dump(NULL, (ProtobufCMessage*)&stream_request, NULL);	
		hangouts_pblite_media_media_stream_add(ha, &stream_request, (HangoutsPbliteMediaStreamAddResponseFunc)hangouts_default_response_dump, NULL);
		
		if (hangouts_media->type & PURPLE_MEDIA_AUDIO) {
			if (audio_stream_otter.n_ssrc) {
				g_free(audio_stream_otter.ssrc);
			}
		}
		if (hangouts_media->type & PURPLE_MEDIA_VIDEO) {
			if (video_stream_otter.n_ssrc_group == 2) {
				g_free(video_stream_otter.ssrc_group[0]->ssrc);
				g_free(video_stream_otter.ssrc_group[1]->ssrc);
				g_free(video_stream_otter.ssrc_group);
			}
			if (video_stream_otter.n_ssrc) {
				g_free(video_stream_otter.ssrc);
			}
		}
		g_free(stream_request.resource);
		hangouts_request_header_free(stream_request.request_header);
	}
	
	//gst debugging:
	//GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(purple_media_manager_get_pipeline(purple_media_manager_get())), GST_DEBUG_GRAPH_SHOW_ALL, "test");
}


static void
hangouts_pblite_media_hangout_resolve_cb(HangoutsAccount *ha, HangoutResolveResponse *response, gpointer user_data)
{
	HangoutsMedia *hangouts_media = user_data;
	PurpleAccount *account = ha->account;
	GParameter *params = NULL;
	guint num_params = 0;
	PurpleMedia *media;
	
	if (hangouts_media == NULL) {
		//wtf
		return;
	}
	
	hangouts_media->hangout_id = g_strdup(response->hangout_id);
	
	purple_debug_info("hangouts", "hangouts_pblite_media_hangout_resolve_cb: ");
	hangouts_default_response_dump(ha, &response->base, user_data);
	
	//TODO use openwebrtc instead of fsrtpconference
	media = purple_media_manager_create_media(purple_media_manager_get(),
			account, "fsrtpconference", hangouts_media->who, TRUE);
	
	if (media == NULL) {
		//TODO do something else
		hangouts_media_destroy(hangouts_media);
		return;
	}
	
	hangouts_media->media = media;
	purple_media_set_protocol_data(media, hangouts_media);
	
	g_signal_connect(G_OBJECT(media), "candidates-prepared",
				 G_CALLBACK(hangouts_media_candidates_prepared_cb), hangouts_media);
	g_signal_connect(G_OBJECT(media), "codecs-changed",
				 G_CALLBACK(hangouts_media_codecs_changed_cb), hangouts_media);
	g_signal_connect(G_OBJECT(media), "state-changed",
				 G_CALLBACK(hangouts_media_state_changed_cb), hangouts_media);
	// TODO
	// g_signal_connect(G_OBJECT(media), "stream-info",
			// G_CALLBACK(hangouts_media_stream_info_cb), hangouts_media);
	
	//TODO add params
	if(hangouts_media->type & PURPLE_MEDIA_AUDIO &&
		!purple_media_add_stream(media, "hangout", hangouts_media->who, hangouts_media->type & PURPLE_MEDIA_AUDIO, TRUE, "nice", num_params, params)) {
		
		purple_media_end(media, NULL, NULL);
		/* TODO: How much clean-up is necessary here? (does calling
				 purple_media_end lead to cleaning up Jingle structs?) */
		return;
	}
	if(hangouts_media->type & PURPLE_MEDIA_VIDEO &&
		!purple_media_add_stream(media, "hangoutv", hangouts_media->who, hangouts_media->type & PURPLE_MEDIA_VIDEO, TRUE, "nice", num_params, params)) {
		
		purple_media_end(media, NULL, NULL);
		return;
	}

#if PURPLE_VERSION_CHECK(2, 10, 12) || PURPLE_VERSION_CHECK(3, 0, 0)
	if (hangouts_media->type & PURPLE_MEDIA_AUDIO &&
		!purple_media_set_send_rtcp_mux(media, "hangout", hangouts_media->who, TRUE)) {
		
		purple_debug_warning("hangouts", "Unable to set rtcp mux on audio stream");
	}
	if (hangouts_media->type & PURPLE_MEDIA_VIDEO &&
		!purple_media_set_send_rtcp_mux(media, "hangoutv", hangouts_media->who, TRUE)) {
			
		purple_debug_warning("hangouts", "Unable to set rtcp mux on video stream");
	}
#endif
	
	//Add self to hangout
	{
		HangoutParticipantAddRequest participant_request;
		HangoutParticipant participant, *participant_ptr = &participant;
		
		hangout_participant_add_request__init(&participant_request);
		hangout_participant__init(&participant);
		
		participant.hangout_id = response->hangout_id;
		participant_request.n_resource = 1;
		participant_request.resource = &participant_ptr;
		participant_request.request_header = hangouts_get_request_header(ha);
		
		purple_debug_info("hangouts", "hangouts_pblite_media_hangout_participant_add: ");
		hangouts_default_response_dump(NULL, (ProtobufCMessage*)&participant_request, NULL);	
		hangouts_pblite_media_hangout_participant_add(ha, &participant_request, hangout_participant_add_cb, hangouts_media);
		
		hangouts_request_header_free(participant_request.request_header);
	}
}

gboolean
hangouts_initiate_media(PurpleAccount *account, const gchar *who, PurpleMediaSessionType type)
{
	PurpleConnection *pc = purple_account_get_connection(account);
	HangoutsAccount *ha = purple_connection_get_protocol_data(pc);
	HangoutsMedia *hangouts_media;
	HangoutResolveRequest request;
	ExternalKey external_key;
	
	hangouts_init_media_functions();
	
	hangouts_media = g_new0(HangoutsMedia, 1);
	hangouts_media->ha = ha;
	hangouts_media->who = g_strdup(who);
	hangouts_media->type = type;
	
	hangout_resolve_request__init(&request);
	external_key__init(&external_key);
	external_key.service = "CONVERSATION";
	external_key.value = g_hash_table_lookup(ha->one_to_ones_rev, who);
	request.external_key = &external_key;
	request.request_header = hangouts_get_request_header(ha);
	hangouts_pblite_media_hangout_resolve(ha, &request, hangouts_pblite_media_hangout_resolve_cb, hangouts_media);
	hangouts_request_header_free(request.request_header);
	
	return TRUE;
}
