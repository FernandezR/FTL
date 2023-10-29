/* Pi-hole: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API Implementation /api/auth
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "api/auth.h"
#include "webserver/http-common.h"
#include "webserver/json_macros.h"
#include "api/api.h"
#include "log.h"
#include "config/config.h"
// get_password_hash()
#include "setupVars.h"
// (un)lock_shm()
#include "shmem.h"
// getrandom()
#include "daemon.h"
// sha256_raw_to_hex()
#include "config/password.h"
// database session functions
#include "database/session-table.h"
// base64_decode()
#include "config/password.h"

static struct session auth_data[API_MAX_CLIENTS] = {{false, false, {false, false}, 0, 0, {0}, {0}, {0}, {0}}};

static void add_request_info(struct ftl_conn *api, const char *csrf)
{
	// Copy CSRF token into request
	if(csrf != NULL)
		strncpy((char*)api->request->csrf_token, csrf, sizeof(api->request->csrf_token) - 1);

	// Store that this client is authenticated
	// We use memset() with the size of an int here to avoid a
	// compiler warning about modifying a variable in a const struct
	memset((int*)&api->request->is_authenticated, 1, sizeof(api->request->is_authenticated));
}

void init_api(void)
{
	// Restore sessions from database
	restore_db_sessions(auth_data);
}

void free_api(void)
{
	// Store sessions in database
	backup_db_sessions(auth_data);
}

// Is this client connecting from localhost?
bool __attribute__((pure)) is_local_api_user(const char *remote_addr)
{
	return strcmp(remote_addr, LOCALHOSTv4) == 0 ||
	       strcmp(remote_addr, LOCALHOSTv6) == 0;
}

// Can we validate this client?
// Returns -1 if not authenticated or expired
// Returns >= 0 for any valid authentication
int check_client_auth(struct ftl_conn *api, const bool is_api)
{
	// Is the user requesting from localhost?
	// This may be allowed without authentication depending on the configuration
	if(!config.webserver.api.localAPIauth.v.b && is_local_api_user(api->request->remote_addr))
	{
		add_request_info(api, NULL);
		return API_AUTH_LOCALHOST;
	}

	// When the pwhash is unset, authentication is disabled
	if(config.webserver.api.pwhash.v.s[0] == '\0')
	{
		add_request_info(api, NULL);
		return API_AUTH_EMPTYPASS;
	}

	// Does the client provide a session ID?
	char sid[SID_SIZE];
	const char *sid_source = "-";
	// Try to extract SID from cookie
	bool sid_avail = false;

	// If not, does the client provide a session ID via GET/POST?
	if(api->payload.avail)
	{
		// Try to extract SID from form-encoded payload
		if(GET_VAR("sid", sid, api->payload.raw) > 0)
		{
			// "+" may have been replaced by " ", undo this here
			for(unsigned int i = 0; i < SID_SIZE; i++)
				if(sid[i] == ' ')
					sid[i] = '+';

			// Zero terminate SID string
			sid[SID_SIZE-1] = '\0';
			// Mention source of SID
			sid_source = "payload (form-data)";
			// Mark SID as available
			sid_avail = true;
		}
		// Try to extract SID from root of a possibly included JSON payload
		else if(api->payload.json != NULL)
		{
			cJSON *sid_obj = cJSON_GetObjectItem(api->payload.json, "sid");
			if(cJSON_IsString(sid_obj))
			{
				// Copy SID string
				strncpy(sid, sid_obj->valuestring, SID_SIZE - 1u);
				// Zero terminate SID string
				sid[SID_SIZE-1] = '\0';
				// Mention source of SID
				sid_source = "payload (JSON)";
				// Mark SID as available
				sid_avail = true;
			}
		}
	}

	// If not, does the client provide a session ID via HEADER?
	if(!sid_avail)
	{
		const char *sid_header = NULL;
		// Try to extract SID from header
		if((sid_header = mg_get_header(api->conn, "sid")) != NULL ||
		   (sid_header = mg_get_header(api->conn, "X-FTL-SID")) != NULL)
		{
			// Copy SID string
			strncpy(sid, sid_header, SID_SIZE - 1u);
			// Zero terminate SID string
			sid[SID_SIZE-1] = '\0';
			// Mention source of SID
			sid_source = "header";
			// Mark SID as available
			sid_avail = true;
		}
	}

	bool cookie_auth = false;
	if(!sid_avail)
	{
		cookie_auth = http_get_cookie_str(api, "sid", sid, SID_SIZE);
		if(cookie_auth)
		{
			// Mention source of SID
			sid_source = "cookie";
			// Mark SID as available
			sid_avail = true;
		}

	}

	if(!sid_avail)
	{
		log_debug(DEBUG_API, "API Authentication: FAIL (no SID provided)");
		return API_AUTH_UNAUTHORIZED;
	}

	// else: Analyze SID
	int user_id = API_AUTH_UNAUTHORIZED;
	const time_t now = time(NULL);
	log_debug(DEBUG_API, "Read sid=\"%s\" from %s", sid, sid_source);

	// If the SID has been sent through a cookie, we require a CSRF token in
	// the header to be sent along with the request for any API requests
	char csrf[SID_SIZE];
	const bool need_csrf = cookie_auth && is_api;
	if(need_csrf)
	{
		const char *csrf_header = NULL;
		// Try to extract CSRF token from header
		if((csrf_header = mg_get_header(api->conn, "X-CSRF-TOKEN")) != NULL)
		{
			// Copy CSRF string
			strncpy(csrf, csrf_header, SID_SIZE - 1u);
			// Zero terminate CSRF string
			csrf[SID_SIZE-1] = '\0';
		}
		else
		{
			log_debug(DEBUG_API, "API Authentication: FAIL (Cookie authentication without CSRF token)");
			return API_AUTH_UNAUTHORIZED;
		}
	}

	for(unsigned int i = 0; i < API_MAX_CLIENTS; i++)
	{
		if(auth_data[i].used &&
		   auth_data[i].valid_until >= now &&
		   strcmp(auth_data[i].remote_addr, api->request->remote_addr) == 0 &&
		   strcmp(auth_data[i].sid, sid) == 0)
		{
			if(need_csrf && strcmp(auth_data[i].csrf, csrf) != 0)
			{
				log_debug(DEBUG_API, "API Authentication: FAIL (CSRF token mismatch, received \"%s\", expected \"%s\")",
				          csrf, auth_data[i].csrf);
				return API_AUTH_UNAUTHORIZED;
			}
			user_id = i;
			break;
		}
	}
	if(user_id > API_AUTH_UNAUTHORIZED)
	{
		// Authentication successful:
		// - We know this client
		// - The session is (still) valid
		// - The IP matches the one we know for this SID

		// Update timestamp of this client to extend
		// the validity of their API authentication
		auth_data[user_id].valid_until = now + config.webserver.session.timeout.v.ui;

		// Set strict_tls permanently to false if the client connected via HTTP
		auth_data[user_id].tls.mixed |= api->request->is_ssl != auth_data[user_id].tls.login;

		// Update user cookie
		if(snprintf(pi_hole_extra_headers, sizeof(pi_hole_extra_headers),
		            FTL_SET_COOKIE,
		            auth_data[user_id].sid, config.webserver.session.timeout.v.ui) < 0)
		{
			return send_json_error(api, 500, "internal_error", "Internal server error", NULL);
		}

		// Add CSRF token to request
		add_request_info(api, auth_data[user_id].csrf);

		// Debug logging
		if(config.debug.api.v.b)
		{
			char timestr[128];
			get_timestr(timestr, auth_data[user_id].valid_until, false, false);
			log_debug(DEBUG_API, "Recognized known user: user_id %i, valid_until: %s, remote_addr %s",
				user_id, timestr, auth_data[user_id].remote_addr);
		}
	}
	else
	{
		log_debug(DEBUG_API, "API Authentication: FAIL (SID invalid/expired)");
		return API_AUTH_UNAUTHORIZED;
	}

	api->user_id = user_id;

	return user_id;
}

static int get_all_sessions(struct ftl_conn *api, cJSON *json)
{
	const time_t now = time(NULL);
	cJSON *sessions = JSON_NEW_ARRAY();
	for(int i = 0; i < API_MAX_CLIENTS; i++)
	{
		if(!auth_data[i].used)
			continue;
		cJSON *session = JSON_NEW_OBJECT();
		JSON_ADD_NUMBER_TO_OBJECT(session, "id", i);
		JSON_ADD_BOOL_TO_OBJECT(session, "current_session", i == api->user_id);
		JSON_ADD_BOOL_TO_OBJECT(session, "valid", auth_data[i].valid_until >= now);
		cJSON *tls = JSON_NEW_OBJECT();
		JSON_ADD_BOOL_TO_OBJECT(tls, "login", auth_data[i].tls.login);
		JSON_ADD_BOOL_TO_OBJECT(tls, "mixed", auth_data[i].tls.mixed);
		JSON_ADD_ITEM_TO_OBJECT(session, "tls", tls);
		JSON_ADD_NUMBER_TO_OBJECT(session, "login_at", auth_data[i].login_at);
		JSON_ADD_NUMBER_TO_OBJECT(session, "last_active", auth_data[i].valid_until - config.webserver.session.timeout.v.ui);
		JSON_ADD_NUMBER_TO_OBJECT(session, "valid_until", auth_data[i].valid_until);
		JSON_REF_STR_IN_OBJECT(session, "remote_addr", auth_data[i].remote_addr);
		JSON_REF_STR_IN_OBJECT(session, "user_agent", auth_data[i].user_agent);
		JSON_ADD_BOOL_TO_OBJECT(session, "app", auth_data[i].app);
		JSON_ADD_ITEM_TO_ARRAY(sessions, session);
	}
	JSON_ADD_ITEM_TO_OBJECT(json, "sessions", sessions);
	return 0;
}

static int get_session_object(struct ftl_conn *api, cJSON *json, const int user_id, const time_t now)
{
	cJSON *session = JSON_NEW_OBJECT();

	// Authentication not needed
	if(user_id == API_AUTH_LOCALHOST || user_id == API_AUTH_EMPTYPASS)
	{
		JSON_ADD_BOOL_TO_OBJECT(session, "valid", true);
		JSON_ADD_BOOL_TO_OBJECT(session, "totp", strlen(config.webserver.api.totp_secret.v.s) > 0);
		JSON_ADD_NULL_TO_OBJECT(session, "sid");
		JSON_ADD_NUMBER_TO_OBJECT(session, "validity", -1);
		JSON_ADD_ITEM_TO_OBJECT(json, "session", session);
		return 0;
	}

	// Valid session
	if(user_id > API_AUTH_UNAUTHORIZED && auth_data[user_id].used)
	{
		JSON_ADD_BOOL_TO_OBJECT(session, "valid", true);
		JSON_ADD_BOOL_TO_OBJECT(session, "totp", strlen(config.webserver.api.totp_secret.v.s) > 0);
		JSON_REF_STR_IN_OBJECT(session, "sid", auth_data[user_id].sid);
		JSON_REF_STR_IN_OBJECT(session, "csrf", auth_data[user_id].csrf);
		JSON_ADD_NUMBER_TO_OBJECT(session, "validity", auth_data[user_id].valid_until - now);
		JSON_ADD_ITEM_TO_OBJECT(json, "session", session);
		return 0;
	}

	// No valid session
	JSON_ADD_BOOL_TO_OBJECT(session, "valid", false);
	JSON_ADD_BOOL_TO_OBJECT(session, "totp", strlen(config.webserver.api.totp_secret.v.s) > 0);
	JSON_ADD_NULL_TO_OBJECT(session, "sid");
	JSON_ADD_NUMBER_TO_OBJECT(session, "validity", -1);
	JSON_ADD_ITEM_TO_OBJECT(json, "session", session);
	return 0;
}

static void delete_session(const int user_id)
{
	// Skip if nothing to be done here
	if(user_id < 0 || user_id >= API_MAX_CLIENTS)
		return;

	// Zero out this session (also sets valid to false == 0)
	memset(&auth_data[user_id], 0, sizeof(auth_data[user_id]));
}

void delete_all_sessions(void)
{
	// Zero out all sessions without looping
	memset(auth_data, 0, sizeof(auth_data));
}

static int send_api_auth_status(struct ftl_conn *api, const int user_id, const time_t now)
{
	if(user_id == API_AUTH_LOCALHOST)
	{
		log_debug(DEBUG_API, "API Auth status: OK (localhost does not need auth)");

		cJSON *json = JSON_NEW_OBJECT();
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT(json);
	}

	if(user_id == API_AUTH_EMPTYPASS)
	{
		log_debug(DEBUG_API, "API Auth status: OK (empty password)");

		cJSON *json = JSON_NEW_OBJECT();
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT(json);
	}

	if(user_id > API_AUTH_UNAUTHORIZED && (api->method == HTTP_GET || api->method == HTTP_POST))
	{
		log_debug(DEBUG_API, "API Auth status: OK");

		// Ten minutes validity
		if(snprintf(pi_hole_extra_headers, sizeof(pi_hole_extra_headers),
		            FTL_SET_COOKIE,
		            auth_data[user_id].sid, config.webserver.session.timeout.d.ui) < 0)
		{
			return send_json_error(api, 500, "internal_error", "Internal server error", NULL);
		}

		cJSON *json = JSON_NEW_OBJECT();
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT(json);
	}
	else if(user_id > API_AUTH_UNAUTHORIZED && api->method == HTTP_DELETE)
	{
		log_debug(DEBUG_API, "API Auth status: Logout, asking to delete cookie");

		// Revoke client authentication. This slot can be used by a new client afterwards.
		delete_session(user_id);

		strncpy(pi_hole_extra_headers, FTL_DELETE_COOKIE, sizeof(pi_hole_extra_headers));
		cJSON *json = JSON_NEW_OBJECT();
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT_CODE(json, 410); // 410 Gone
	}
	else
	{
		log_debug(DEBUG_API, "API Auth status: Invalid, asking to delete cookie");

		strncpy(pi_hole_extra_headers, FTL_DELETE_COOKIE, sizeof(pi_hole_extra_headers));
		cJSON *json = JSON_NEW_OBJECT();
		get_session_object(api, json, user_id, now);
		JSON_SEND_OBJECT_CODE(json, 401); // 401 Unauthorized
	}
}

static void generateSID(char *sid)
{
	uint8_t raw_sid[SID_SIZE];
	if(getrandom(raw_sid, sizeof(raw_sid), 0) < 0)
	{
		log_err("getrandom() failed in generateSID()");
		return;
	}
	base64_encode_raw(NETTLE_SIGN sid, SID_BITSIZE/8, raw_sid);
	sid[SID_SIZE-1] = '\0';
}

static char *basic_auth(struct ftl_conn *api)
{
	const char *auth_header = mg_get_header(api->conn, "Authorization");
	if(auth_header == NULL)
		return NULL;

	// Check if this is a Basic Auth header
	if(strncmp(auth_header, "Basic ", 6) != 0)
		return NULL;

	// Decode Base64
	size_t length = 0;
	char *decoded = (char*)base64_decode(auth_header + 6, &length);
	if(decoded == NULL)
		return NULL;

	// Extract username and password
	char *username = decoded;
	char *password = strchr(decoded, ':');
	if(password == NULL)
	{
		free(decoded);
		return NULL;
	}
	*password = '\0';
	password++;

	// Check if username is correct
	if(strcmp(username, "pi-hole") != 0)
	{
		free(decoded);
		return NULL;
	}

	char *password_copy = strdup(password);
	free(decoded);

	// Return copy of password
	return password_copy;
}

// api/auth
//  GET: Check authentication
//  POST: Login
//  DELETE: Logout
int api_auth(struct ftl_conn *api)
{
	// Check HTTP method
	char *password = NULL;
	const time_t now = time(NULL);
	const bool empty_password = config.webserver.api.pwhash.v.s[0] == '\0';

	if(api->item != NULL && strlen(api->item) > 0)
	{
		// Sub-paths are not allowed
		return 0;
	}

	// Did the client authenticate before and we can validate this?
	int user_id = check_client_auth(api, false);

	// Login attempt, check password
	if(api->method == HTTP_POST)
	{
		// Try to extract response from payload
		if (api->payload.json == NULL)
		{
			if (api->payload.json_error == NULL)
				return send_json_error(api, 400,
				                       "bad_request",
				                       "No request body data",
				                       NULL);
			else
				return send_json_error(api, 400,
				                       "bad_request",
				                       "Invalid request body data (no valid JSON), error before hint",
				                       api->payload.json_error);
		}

		// Check if password is available
		cJSON *json_password;
		if((json_password = cJSON_GetObjectItemCaseSensitive(api->payload.json, "password")) == NULL)
		{
			const char *message = "No password found in JSON payload";
			log_debug(DEBUG_API, "API auth error: %s", message);
			return send_json_error(api, 400,
			                       "bad_request",
			                       message,
			                       NULL);
		}

		// Check password type
		if(!cJSON_IsString(json_password))
		{
			const char *message = "Field password has to be of type 'string'";
			log_debug(DEBUG_API, "API auth error: %s", message);
			return send_json_error(api, 400,
			                       "bad_request",
			                       message,
			                       NULL);
		}

		// password is already null-terminated
		password = json_password->valuestring;
	}

	// If there is no password, check if user provided a password via HTTP Basic Auth
	if(password == NULL || strlen(password) == 0)
		password = basic_auth(api);

	// If this is a valid session, we can exit early at this point if no password is supplied
	if(user_id != API_AUTH_UNAUTHORIZED && (password == NULL || strlen(password) == 0))
		return send_api_auth_status(api, user_id, now);

	// Logout attempt
	if(api->method == HTTP_DELETE)
	{
		log_debug(DEBUG_API, "API Auth: User with ID %i wants to log out", user_id);
		return send_api_auth_status(api, user_id, now);
	}

	// If this is not a login attempt, we can exit early at this point
	if(password == NULL && !empty_password)
		return send_api_auth_status(api, user_id, now);

	// else: Login attempt
	// - Client tries to authenticate using a password, or
	// - There no password on this machine
	enum password_result result = PASSWORD_INCORRECT;

	// If there is no password (or empty), check if there is any password at all
	log_info("Password: \"%s\"", password);
	if(empty_password && (password == NULL || strlen(password) == 0))
		result = PASSWORD_CORRECT;
	else
		result = verify_login(password);

	if(result == PASSWORD_CORRECT || result == APPPASSWORD_CORRECT)
	{
		// Accepted

		// Zero-out password in memory to avoid leaking it when it is
		// freed at the end of the current API request
		if(password != NULL)
			memset(password, 0, strlen(password));

		// Check possible 2FA token
		// Successful login with empty password does not require 2FA
		if(strlen(config.webserver.api.totp_secret.v.s) > 0 && result != APPPASSWORD_CORRECT)
		{
			// Get 2FA token from payload
			cJSON *json_totp;
			if((json_totp = cJSON_GetObjectItemCaseSensitive(api->payload.json, "totp")) == NULL)
			{
				const char *message = "No 2FA token found in JSON payload";
				log_debug(DEBUG_API, "API auth error: %s", message);
				return send_json_error(api, 400,
							"bad_request",
							message,
							NULL);
			}

			if(!verifyTOTP(json_totp->valueint))
			{
				// 2FA token is invalid
				return send_json_error(api, 401,
							"unauthorized",
							"Invalid 2FA token",
							NULL);
			}
		}

		// Find unused authentication slot
		for(unsigned int i = 0; i < API_MAX_CLIENTS; i++)
		{
			// Expired slow, mark as unused
			if(auth_data[i].used &&
				auth_data[i].valid_until < now)
			{
				log_debug(DEBUG_API, "API: Session of client %u (%s) expired, freeing...",
						i, auth_data[i].remote_addr);
				delete_session(i);
			}

			// Found unused authentication slot (might have been freed before)
			if(!auth_data[i].used)
			{
				// Mark as used
				auth_data[i].used = true;
				// Set validitiy to now + timeout
				auth_data[i].login_at = now;
				auth_data[i].valid_until = now + config.webserver.session.timeout.v.ui;
				// Set remote address
				strncpy(auth_data[i].remote_addr, api->request->remote_addr, sizeof(auth_data[i].remote_addr));
				auth_data[i].remote_addr[sizeof(auth_data[i].remote_addr)-1] = '\0';
				// Store user-agent (if available)
				const char *user_agent = mg_get_header(api->conn, "user-agent");
				if(user_agent != NULL)
				{
					strncpy(auth_data[i].user_agent, user_agent, sizeof(auth_data[i].user_agent));
					auth_data[i].user_agent[sizeof(auth_data[i].user_agent)-1] = '\0';
				}
				else
				{
					auth_data[i].user_agent[0] = '\0';
				}

				auth_data[i].tls.login = api->request->is_ssl;
				auth_data[i].tls.mixed = false;
				auth_data[i].app = result == APPPASSWORD_CORRECT;

				// Generate new SID and CSRF token
				generateSID(auth_data[i].sid);
				generateSID(auth_data[i].csrf);

				user_id = i;
				break;
			}
		}

		// Debug logging
		if(config.debug.api.v.b && user_id > API_AUTH_UNAUTHORIZED)
		{
			char timestr[128];
			get_timestr(timestr, auth_data[user_id].valid_until, false, false);
			log_debug(DEBUG_API, "API: Registered new user: user_id %i valid_until: %s remote_addr %s (accepted due to %s)",
					user_id, timestr, auth_data[user_id].remote_addr,
					empty_password ? "empty password" : "correct response");
		}
		if(user_id == API_AUTH_UNAUTHORIZED)
		{
			log_warn("No free API seats available, not authenticating client");
		}
	}
	else if(result == PASSWORD_RATE_LIMITED)
	{
		// Rate limited
		return send_json_error(api, 429,
					"too_many_requests",
					"Too many requests",
					"login rate limiting");
	}
	else
	{
		log_debug(DEBUG_API, "API: Password incorrect: '%s'", password);
	}

	// Free allocated memory
	return send_api_auth_status(api, user_id, now);
}

int api_auth_sessions(struct ftl_conn *api)
{
	// Get session object
	cJSON *json = JSON_NEW_OBJECT();
	get_all_sessions(api, json);
	JSON_SEND_OBJECT(json);
}

int api_auth_session_delete(struct ftl_conn *api)
{
	// Get user ID
	int uid;
	if(sscanf(api->item, "%i", &uid) != 1)
		return send_json_error(api, 400, "bad_request", "Missing or invalid session ID", NULL);

	// Check if session ID is valid
	if(uid <= API_AUTH_UNAUTHORIZED || uid >= API_MAX_CLIENTS)
		return send_json_error(api, 400, "bad_request", "Session ID out of bounds", NULL);

	// Check if session is used
	if(!auth_data[uid].used)
		return send_json_error(api, 400, "bad_request", "Session ID not in use", NULL);

	// Delete session
	delete_session(uid);

	// Send empty reply with code 204 No Content
	send_http_code(api, "application/json; charset=utf-8", 204, "");
	return 204;
}
