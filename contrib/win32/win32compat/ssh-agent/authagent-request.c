/*
* Author: Manoj Ampalam <manoj.ampalam@microsoft.com>
* ssh-agent implementation on Windows
*
* Copyright (c) 2015 Microsoft Corp.
* All rights reserved
*
* Microsoft openssh win32 port
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
* NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
* THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define UMDF_USING_NTSTATUS 
#include <Windows.h>
#include <UserEnv.h>
#include <Ntsecapi.h>
#include <ntstatus.h>
#include <Shlobj.h>
#include "agent.h"
#include "agent-request.h"
#include "key.h"
#include "inc\utf.h"

static void
InitLsaString(LSA_STRING *lsa_string, const char *str)
{
	if (str == NULL)
		memset(lsa_string, 0, sizeof(LSA_STRING));
	else {
		lsa_string->Buffer = (char *)str;
		lsa_string->Length = strlen(str);
		lsa_string->MaximumLength = lsa_string->Length + 1;
	}
}

static void
EnablePrivilege(const char *privName, int enabled)
{
	TOKEN_PRIVILEGES tp;
	HANDLE hProcToken = NULL;
	LUID luid;

	int exitCode = 1;

	if (LookupPrivilegeValueA(NULL, privName, &luid) == FALSE ||
		OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hProcToken) == FALSE)
		goto done;

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = enabled ? SE_PRIVILEGE_ENABLED : 0;

	AdjustTokenPrivileges(hProcToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);

done:
	if (hProcToken)
		CloseHandle(hProcToken);

	return;
}


void
LoadProfile(struct agent_connection* con, wchar_t* user, wchar_t* domain) {
	PROFILEINFOW profileInfo;
	profileInfo.dwFlags = PI_NOUI;
	profileInfo.lpProfilePath = NULL;
	profileInfo.lpUserName = user;
	profileInfo.lpDefaultPath = NULL;
	profileInfo.lpServerName = domain;
	profileInfo.lpPolicyPath = NULL;
	profileInfo.hProfile = NULL;
	profileInfo.dwSize = sizeof(profileInfo);
	EnablePrivilege("SeBackupPrivilege", 1);
	EnablePrivilege("SeRestorePrivilege", 1);
	if (LoadUserProfileW(con->auth_token, &profileInfo) == FALSE)
		debug("Loading user (%ls,%ls) profile failed ERROR: %d", user, domain, GetLastError());
	else
		con->hProfile = profileInfo.hProfile;
	EnablePrivilege("SeBackupPrivilege", 0);
	EnablePrivilege("SeRestorePrivilege", 0);
}

#define MAX_USER_LEN 64
/* https://technet.microsoft.com/en-us/library/active-directory-maximum-limits-scalability(v=ws.10).aspx */
#define MAX_FQDN_LEN 64 
#define MAX_PW_LEN 64

static HANDLE
generate_user_token(wchar_t* user_cpn) {
	HANDLE lsa_handle = 0, token = 0;
	LSA_OPERATIONAL_MODE mode;
	ULONG auth_package_id;
	NTSTATUS ret, subStatus;
	void * logon_info = NULL;
	size_t logon_info_size;
	LSA_STRING logon_process_name, auth_package_name, originName;
	TOKEN_SOURCE sourceContext;
	PKERB_INTERACTIVE_PROFILE pProfile = NULL;
	LUID logonId;
	QUOTA_LIMITS quotas;
	DWORD cbProfile;
	BOOL domain_user;

	domain_user = wcschr(user_cpn, L'@')? TRUE : FALSE;

	InitLsaString(&logon_process_name, "ssh-agent");
	if (domain_user)
		InitLsaString(&auth_package_name, MICROSOFT_KERBEROS_NAME_A);
	else
		InitLsaString(&auth_package_name, "SSH-LSA");

	InitLsaString(&originName, "sshd");
	if (ret = LsaRegisterLogonProcess(&logon_process_name, &lsa_handle, &mode) != STATUS_SUCCESS)
		goto done;

	if (ret = LsaLookupAuthenticationPackage(lsa_handle, &auth_package_name, &auth_package_id) != STATUS_SUCCESS)
		goto done;

	if (domain_user) {
		KERB_S4U_LOGON *s4u_logon;
		logon_info_size = sizeof(KERB_S4U_LOGON);
		logon_info_size += (wcslen(user_cpn) * 2 + 2);
		logon_info = malloc(logon_info_size);
		if (logon_info == NULL)
			goto done;
		s4u_logon = (KERB_S4U_LOGON*)logon_info;
		s4u_logon->MessageType = KerbS4ULogon;
		s4u_logon->Flags = 0;
		s4u_logon->ClientUpn.Length = wcslen(user_cpn) * 2;
		s4u_logon->ClientUpn.MaximumLength = s4u_logon->ClientUpn.Length;
		s4u_logon->ClientUpn.Buffer = (WCHAR*)(s4u_logon + 1);
		memcpy(s4u_logon->ClientUpn.Buffer, user_cpn, s4u_logon->ClientUpn.Length + 2);
		s4u_logon->ClientRealm.Length = 0;
		s4u_logon->ClientRealm.MaximumLength = 0;
		s4u_logon->ClientRealm.Buffer = 0;
	} else {
		logon_info_size = (wcslen(user_cpn) + 1)*sizeof(wchar_t);
		logon_info = malloc(logon_info_size);
		if (logon_info == NULL)
			goto done;
		memcpy(logon_info, user_cpn, logon_info_size);
	}

	memcpy(sourceContext.SourceName,"sshagent", sizeof(sourceContext.SourceName));

	if (AllocateLocallyUniqueId(&sourceContext.SourceIdentifier) != TRUE)
		goto done;

	if (ret = LsaLogonUser(lsa_handle,
		&originName,
		Network,
		auth_package_id,
		logon_info,
		logon_info_size,
		NULL,
		&sourceContext,
		(PVOID*)&pProfile,
		&cbProfile,
		&logonId,
		&token,
		&quotas,
		&subStatus) != STATUS_SUCCESS) {
		debug("LsaLogonUser failed %d", ret);
		goto done;
	}
	debug3("LsaLogonUser succeeded");
done:
	if (lsa_handle)
		LsaDeregisterLogonProcess(lsa_handle);
	if (logon_info)
		free(logon_info);
	if (pProfile)
		LsaFreeReturnBuffer(pProfile);

	return token;
}

/* TODO - SecureZeroMemory password */
int process_passwordauth_request(struct sshbuf* request, struct sshbuf* response, struct agent_connection* con) {
	char *user = NULL, *domain = NULL, *pwd = NULL;
	size_t user_len, pwd_len;
	wchar_t *user_utf16 = NULL, *udom_utf16 = NULL, *pwd_utf16 = NULL, *tmp;
	int r = -1;
	HANDLE token = 0, dup_token, client_proc = 0;
	ULONG client_pid;

	if (sshbuf_get_cstring(request, &user, &user_len) != 0 ||
	    sshbuf_get_cstring(request, &pwd, &pwd_len) != 0 ||
	    user_len == 0 ||
	    pwd_len == 0 ||
	    user_len > MAX_USER_LEN + MAX_FQDN_LEN ||
	    pwd_len > MAX_PW_LEN) {
		debug("bad password auth request");
		goto done;
	}

	if ((user_utf16 = utf8_to_utf16(user)) == NULL ||
	    (pwd_utf16 = utf8_to_utf16(pwd)) == NULL) {
		debug("out of memory");
		goto done;
	}

	if ((tmp = wcschr(user_utf16, L'@') ) != NULL ) {
		udom_utf16 = tmp + 1;
		*tmp = L'\0';
	}

	if (LogonUserW(user_utf16, udom_utf16, pwd_utf16, LOGON32_LOGON_NETWORK_CLEARTEXT, LOGON32_PROVIDER_DEFAULT, &token) == FALSE) {
		debug("failed to logon user: %ls domain: %ls", user_utf16, udom_utf16);
		goto done;
	}

	if ((FALSE == GetNamedPipeClientProcessId(con->connection, &client_pid)) ||
	    ((client_proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid)) == NULL) ||
	    (FALSE == DuplicateHandle(GetCurrentProcess(), token, client_proc, &dup_token, TOKEN_QUERY | TOKEN_IMPERSONATE, FALSE, DUPLICATE_SAME_ACCESS)) ||
	    (sshbuf_put_u32(response, (int)(intptr_t)dup_token) != 0)) {
		debug("failed to duplicate user token");
		goto done;
	}

	con->auth_token = token;
	LoadProfile(con, user_utf16, udom_utf16);
	r = 0;
done:
	/* TODO Fix this hacky protocol*/
	if ((r == -1) && (sshbuf_put_u8(response, SSH_AGENT_FAILURE) == 0))
		r = 0;

	if (user)
		free(user);
	if (pwd)
		free(pwd);
	if (user_utf16)
		free(user_utf16);
	if (pwd_utf16)
		free(pwd_utf16);
	if (client_proc)
		CloseHandle(client_proc);

	return r;
}

int process_pubkeyauth_request(struct sshbuf* request, struct sshbuf* response, struct agent_connection* con) {
	int r = -1;
	char *key_blob, *user, *sig, *blob;
	size_t key_blob_len, user_len, sig_len, blob_len;
	struct sshkey *key = NULL;
	HANDLE token = NULL, restricted_token = NULL, dup_token = NULL, client_proc = NULL;
	wchar_t *user_utf16 = NULL, *udom_utf16 = NULL, *tmp;
	PWSTR wuser_home = NULL;
	ULONG client_pid;
	LUID_AND_ATTRIBUTES priv_to_delete[1];

	user = NULL;
	if (sshbuf_get_string_direct(request, &key_blob, &key_blob_len) != 0 ||
	    sshbuf_get_cstring(request, &user, &user_len) != 0 ||
	    user_len > MAX_USER_LEN ||
	    sshbuf_get_string_direct(request, &sig, &sig_len) != 0 ||
	    sshbuf_get_string_direct(request, &blob, &blob_len) != 0 ||
	    sshkey_from_blob(key_blob, key_blob_len, &key) != 0) {
		debug("invalid pubkey auth request");
		goto done;
	}

	if ((user_utf16 = utf8_to_utf16(user)) == NULL) {
		debug("out of memory");
		goto done;
	}

	if ((token = generate_user_token(user_utf16)) == 0) {
		debug("unable to generate token for user %ls", user_utf16);
		goto done;
	}

	/* for key based auth, remove SeTakeOwnershipPrivilege */
	if (LookupPrivilegeValueW(NULL, L"SeTakeOwnershipPrivilege", &priv_to_delete[0].Luid) == FALSE ||
	    CreateRestrictedToken(token, 0, 0, NULL, 1, priv_to_delete, 0, NULL, &restricted_token) == FALSE) {
		debug("unable to remove SeTakeOwnershipPrivilege privilege");
		goto done;
	}
	
	if (SHGetKnownFolderPath(&FOLDERID_Profile, 0, restricted_token, &wuser_home) != S_OK ||
	    pubkey_allowed(key, user_utf16, wuser_home) != 1) {
		debug("unable to verify public key for user %ls (profile:%ls)", user_utf16, wuser_home);
		goto done;
	}

	if (key_verify(key, sig, sig_len, blob, blob_len) != 1) {
		debug("signature verification failed");
		goto done;
	}

	if ((FALSE == GetNamedPipeClientProcessId(con->connection, &client_pid)) ||
	    ( (client_proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid)) == NULL) ||
	    (FALSE == DuplicateHandle(GetCurrentProcess(), restricted_token, client_proc, &dup_token, TOKEN_QUERY | TOKEN_IMPERSONATE, FALSE, DUPLICATE_SAME_ACCESS)) ||
	    (sshbuf_put_u32(response, (int)(intptr_t)dup_token) != 0)) {
		debug("failed to authorize user");
		goto done;
	}

	con->auth_token = restricted_token; 
	restricted_token = NULL;
	if ((tmp = wcschr(user_utf16, L'@')) != NULL) {
		udom_utf16 = tmp + 1;
		*tmp = L'\0';
	}
	LoadProfile(con, user_utf16, udom_utf16);

	r = 0;
done:
	/* TODO Fix this hacky protocol*/
	if ((r == -1) && (sshbuf_put_u8(response, SSH_AGENT_FAILURE) == 0))
		r = 0;

	if (user)
		free(user);
	if (user_utf16)
		free(user_utf16);
	if (key)
		sshkey_free(key);
	if (wuser_home)
		CoTaskMemFree(wuser_home);
	if (client_proc)
		CloseHandle(client_proc);
	if (token)
		CloseHandle(token);
	return r;
}

int process_authagent_request(struct sshbuf* request, struct sshbuf* response, struct agent_connection* con) {
	char *opn;
	size_t opn_len;
	if (sshbuf_get_string_direct(request, &opn, &opn_len) != 0) {
		debug("invalid auth request");
		return -1;
	}

	if (memcmp(opn, PUBKEY_AUTH_REQUEST, opn_len) == 0)
		return process_pubkeyauth_request(request, response, con);
	else if (memcmp(opn, PASSWD_AUTH_REQUEST, opn_len) == 0)
		return process_passwordauth_request(request, response, con);
	else {
		debug("unknown auth request: %s", opn);
		return -1;
	}
}