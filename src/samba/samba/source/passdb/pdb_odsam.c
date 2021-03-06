/*
   Unix SMB/CIFS implementation.
   passdb opendirectory backend

   Copyright (c) 2003-2007 Apple Inc. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#define USES_ODGROUPMAPPING 1

#define BOOL_DEFINED

#include "includes.h"

#define USE_SETATTRIBUTEVALUE 1
#define AUTH_GET_POLICY 0
#define GLOBAL_DEFAULT 1
#define WITH_PASSWORD_HASH 1

static int odssam_debug_level = DBGC_ALL;
static int module_debug;
static uid_t guest_uid = (uid_t)-1;

#undef DBGC_CLASS
#define DBGC_CLASS odssam_debug_level

#define MODULE_NAME "odsam"

#include "opendirectory.h"

struct odssam_privates {
	struct opendirectory_session session;

	/* saved state from last search */
	CFMutableArrayRef	usersArray;
	CFMutableArrayRef	groupsArray;

	int usersIndex;
	int groupsIndex;
	tContextData contextData;
	tContextData localContextData;
};

static enum ds_trace_level ds_trace = DS_TRACE_ERRORS;

static struct odssam_privates *get_ods_state(struct pdb_methods *methods)
{
	struct odssam_privates *ods_state;
	tDirStatus status;
	tDirNodeReference dirRef;


	ods_state = (struct odssam_privates *)methods->private_data;
	SMB_ASSERT(ods_state != NULL);

	dirRef = ods_state->session.ref;
	status = opendirectory_reconnect(&ods_state->session);
	if (status != eDSNoErr) {
		/* Unable to restore our connection to the directory. We are
		 * toast for (at least) this call.
		 */
		LOG_DS_ERROR(ds_trace, status, "opendirectory_reconnect");
		return NULL;
	}

	return ods_state;
}

static uint32_t add_data_buffer_item(tDataBufferPtr dataBuffer,
			size_t len, const void *buffer)
{
	uint32_t result = 0;

	memcpy(&(dataBuffer->fBufferData[ dataBuffer->fBufferLength ]), &len, 4);
	dataBuffer->fBufferLength += 4;
	if (len != 0) {
		memcpy(&(dataBuffer->fBufferData[ dataBuffer->fBufferLength ]), buffer, len);
		if ((dataBuffer->fBufferLength+len) > INT32_MAX)
		    smb_panic("PANIC: Looks like fBufferLength overflowed\n");
		dataBuffer->fBufferLength += (uint32_t)len;
	}

	return result;
}

static BOOL GetCString(CFStringRef cfstr, char *outbuffer, int size)
{
    if (cfstr) {
        return CFStringGetCString(cfstr, outbuffer,
		    size, kCFStringEncodingUTF8);
    }

    return False;
}
static BOOL is_machine_name(const char *name)
{
	if (name) {
		return (name[strlen (name) -1] == '$');
	}

	return False;
}
const const char *get_record_type(const char *name)
{
	if (is_machine_name(name)) {
		return kDSStdRecordTypeComputers;
	} else {
		return kDSStdRecordTypeUsers;
	}
}

static tDirStatus odssam_open(struct odssam_privates *ods_state)
{
	tDirStatus dirStatus;

	dirStatus = opendirectory_reconnect(&ods_state->session);
	LOG_DS_ERROR_MSG(ds_trace, dirStatus, "dsOpenDirService",
		    ("refnum=%lu\n", (unsigned long)ods_state->session.ref));

	return dirStatus;
}

static tDirStatus get_password_policy(struct odssam_privates *ods_state,
		tDirNodeReference pwsNode,
		const char *userid, char *policy,
		const char *type)
{
	tDirStatus 		status			= eDSNoErr;
	UInt32			len			= 0;
	tDataBufferPtr		authBuff  		= NULL;
	tDataBufferPtr		stepBuff  		= NULL;
	tDataNodePtr		authType		= NULL;
	tDataNodePtr		recordType		= NULL;

        authBuff = dsDataBufferAllocate(ods_state->session.ref,
					DEFAULT_DS_BUFFER_SIZE);
	stepBuff = dsDataBufferAllocate(ods_state->session.ref,
					DEFAULT_DS_BUFFER_SIZE);
        if (authBuff == NULL || stepBuff == NULL) {
		goto cleanup;
	}

	authType = dsDataNodeAllocateString(ods_state->session.ref,
			    /*kDSStdAuthGetEffectivePolicy*/
			    "dsAuthMethodStandard:dsAuthGetEffectivePolicy");
	recordType = dsDataNodeAllocateString(ods_state->session.ref,  type);
	if (authType == NULL) {
		goto cleanup;
	}

	// User Name (target account)
	add_data_buffer_item(authBuff, strlen(userid), userid);

	status = dsDoDirNodeAuthOnRecordType(pwsNode, authType, True,
				authBuff, stepBuff, NULL, recordType);
	LOG_DS_ERROR_MSG(ds_trace, status, "dsDoDirNodeAuthOnRecordType",
		("kDSStdAuthGetEffectivePolicy for user  \"%s\"\n", userid));
	if (status == eDSNoErr) {
		memcpy(&len, stepBuff->fBufferData, 4);
		stepBuff->fBufferData[len+4] = '\0';
		safe_strcpy(policy,stepBuff->fBufferData+4, 1024);
	}

cleanup:
	opendirectory_free_node(&ods_state->session, authType);
	opendirectory_free_node(&ods_state->session, recordType);
	opendirectory_free_buffer(&ods_state->session, authBuff);
	opendirectory_free_buffer(&ods_state->session, stepBuff);

	return status;
}

static tDirStatus set_password_policy(struct odssam_privates *ods_state,
		tDirNodeReference pwsNode,
		const char *userid,
		const char *policy,
		const char *type)
{
	tDirStatus 		status			= eDSNoErr;
	tDataBufferPtr		authBuff  		= NULL;
	tDataBufferPtr		stepBuff  		= NULL;
	tDataNodePtr		authType		= NULL;
	tDataNodePtr		recordType		= NULL;


	authBuff = dsDataBufferAllocate(ods_state->session.ref,
		DEFAULT_DS_BUFFER_SIZE);
	if (authBuff != NULL)
	{
			stepBuff = dsDataBufferAllocate(ods_state->session.ref,
				DEFAULT_DS_BUFFER_SIZE);
			if (stepBuff != NULL)
			{
					authType =
					    dsDataNodeAllocateString(ods_state->session.ref,  "dsAuthMethodStandard:dsAuthSetPolicyAsRoot" /*kDSStdAuthSetPolicyAsRoot*/);
					recordType =
					    dsDataNodeAllocateString(ods_state->session.ref,  type);
				   if (authType != NULL)
					{
							add_data_buffer_item(authBuff, strlen(userid), userid);
							add_data_buffer_item(authBuff, strlen(policy), policy);

							status = dsDoDirNodeAuthOnRecordType(pwsNode, authType, True, authBuff, stepBuff, NULL, recordType);
							if (status == eDSNoErr)
							{
									DEBUG(module_debug,("kDSStdAuthSetPolicyAsRoot was successful for user  \"%s\" :)\n", userid));
							}
							else
							{
									DEBUG(module_debug,("kDSStdAuthSetPolicyAsRoot FAILED for user \"%s\" (%d) :(\n", userid, status));
							}
					}
			}
			else
			{
					DEBUG(module_debug,("set_password_policy: *** dsDataBufferAllocate(2) faild with \n"));
			}
	}
	else
	{
			DEBUG(module_debug,("set_password_policy: *** dsDataBufferAllocate(1) faild with \n"));
	}

	opendirectory_free_node(&ods_state->session, authType);
	opendirectory_free_node(&ods_state->session, recordType);
   	opendirectory_free_buffer(&ods_state->session, authBuff);
	opendirectory_free_buffer(&ods_state->session, stepBuff);

	return status;
}

static tDirStatus set_password(struct odssam_privates *ods_state,
		tDirNodeReference userNode,
		const char *user,
		const char *passwordstring,
		const char *passwordType,
		const char *type)
{
	tDirStatus 		status			= eDSNullParameter;
	tDataBufferPtr	authBuff  		= NULL;
	tDataBufferPtr	stepBuff  		= NULL;
	tDataNodePtr	authType		= NULL;
	tDataNodePtr	recordType		= NULL;
	const char	*password		= NULL;
	unsigned long	passwordLen		= 0;
#if defined(kDSStdAuthSetWorkstationPasswd) && defined(kDSStdAuthSetLMHash)
	uint8			binarypwd[NT_HASH_LEN];
#endif

	if (strcmp(passwordType, kDSStdAuthSetPasswdAsRoot) == 0) {
		password = passwordstring;
		passwordLen = strlen(password);
#if defined(kDSStdAuthSetWorkstationPasswd) && defined(kDSStdAuthSetLMHash)
	} else if (strcmp(passwordType, kDSStdAuthSetWorkstationPasswd) == 0 || strcmp(passwordType, kDSStdAuthSetLMHash) == 0) {
		if (pdb_gethexpwd(passwordstring, binarypwd)) {
			password = (const char*)binarypwd;
			passwordLen = NT_HASH_LEN;
		}
#endif
	} else {
		return status;
	}

	authBuff = dsDataBufferAllocate(ods_state->session.ref,
		DEFAULT_DS_BUFFER_SIZE);
	if (authBuff != NULL  && password && passwordLen)
	{
			stepBuff = dsDataBufferAllocate(ods_state->session.ref,
				DEFAULT_DS_BUFFER_SIZE);
			if (stepBuff != NULL)
			{
					authType =
					    dsDataNodeAllocateString(ods_state->session.ref,  passwordType);
					 recordType =
					     dsDataNodeAllocateString(ods_state->session.ref,  type);
				   if (authType != NULL)
					{
							// User Name
							add_data_buffer_item(authBuff, strlen(user), user);
							// Password
							add_data_buffer_item(authBuff, passwordLen, password);
							DEBUG(module_debug,("set_password len (%ld), password (%s) \n", passwordLen, password));

							status = dsDoDirNodeAuthOnRecordType(userNode, authType, True, authBuff, stepBuff, NULL, recordType);
							if (status == eDSNoErr)
							{
									DEBUG(module_debug,("Set password (%s) was successful for account  \"%s\" accountType (%s) :)\n", passwordType, user, type));
							}
							else
							{
									DEBUG(module_debug,("Set password (%s) FAILED for account \"%s\" accountType (%s) (%d) :(\n", passwordType, user, type, status));
							}
					}
			}
			else
			{
					DEBUG(module_debug,("set_password: *** dsDataBufferAllocate(2) faild with \n"));
			}
	}
	else
	{
			DEBUG(module_debug,("set_password: *** dsDataBufferAllocate(1) faild with \n"));
	}


	opendirectory_free_buffer(&ods_state->session, authBuff);
	opendirectory_free_buffer(&ods_state->session, stepBuff);
	opendirectory_free_node(&ods_state->session, authType);
	opendirectory_free_node(&ods_state->session, recordType);

	return status;
}

static tDirStatus get_record_ref(struct odssam_privates *ods_state,
			    tDirNodeReference nodeReference,
			    tRecordReference *ref,
			    const char *recordType,
			    const char *recordName)
{
	tDirStatus status = eDSNoErr;
	tDataNodePtr recordNameNode = NULL;
	tDataNodePtr recordTypeNode = NULL;

	recordNameNode = dsDataNodeAllocateString(ods_state->session.ref,
						    recordName);
	recordTypeNode = dsDataNodeAllocateString(ods_state->session.ref,
						    recordType);
	status = dsOpenRecord(nodeReference, recordTypeNode,
				recordNameNode, ref);
	LOG_DS_ERROR(ds_trace, status, "dsOpenRecord");

	opendirectory_free_node(&ods_state->session, recordNameNode);
	opendirectory_free_node(&ods_state->session, recordTypeNode);

	return status;
}

static tDirStatus set_recordname(struct odssam_privates *ods_state,
			    tRecordReference recordReference,
			    const char *value)
{
	tDirStatus status = eDSNullParameter;
	tDataNodePtr attributeValue = NULL;

	if (value && *value) {
		attributeValue =
		    dsDataNodeAllocateString(ods_state->session.ref, value);
		status = dsSetRecordName(recordReference, attributeValue);
		LOG_DS_ERROR(ds_trace, status, "dsSetRecordName");
	}
	opendirectory_free_node(&ods_state->session, attributeValue);
	return status;
}

static BOOL isPWSAttribute(const char *attribute)
{
	BOOL result = false;

	if ((strcmp(attribute, kPWSIsDisabled) == 0) ||
		(strcmp(attribute, kPWSIsAdmin) == 0) ||
		(strcmp(attribute, kPWSExpiryDateEnabled) == 0) ||
		(strcmp(attribute, kPWSNewPasswordRequired) == 0) ||
		(strcmp(attribute, kPWSIsUsingHistory) == 0) ||
		(strcmp(attribute, kPWSCanChangePassword) == 0) ||
		(strcmp(attribute, kPWSExpiryDateEnabled) == 0) ||
		(strcmp(attribute, kPWSRequiresAlpha) == 0) ||
		(strcmp(attribute, kPWSExpiryDate) == 0) ||
		(strcmp(attribute, kPWSHardExpiryDate) == 0) ||
		(strcmp(attribute, kPWSMaxMinChgPwd) == 0) ||
		(strcmp(attribute, kPWSMaxMinActive) == 0) ||
		(strcmp(attribute, kPWSMaxFailedLogins) == 0) ||
		(strcmp(attribute, kPWSMinChars) == 0) ||
		(strcmp(attribute, kPWSMaxChars) == 0) ||
		(strcmp(attribute, kPWSPWDCannotBeName) == 0) ||
		(strcmp(attribute, kPWSPWDLastSetTime) == 0) ||
		(strcmp(attribute, kPWSLastLoginTime) == 0) ||
		(strcmp(attribute, kPWSLogOffTime) == 0) ||
		(strcmp(attribute, kPWSKickOffTime) == 0))
			result = true;

	return result;
}

static void add_password_policy_attribute(char *policy,
		    const char *attribute, const char *value)
{
	char *entry = NULL;

	/* XXX: should check for overflow of "policy" */
	entry = policy + strlen(policy);
	snprintf(entry, strlen(policy),"%s= %s ", attribute, value);
}

static tDirStatus add_attribute_with_value(struct odssam_privates *ods_state,
			    tRecordReference recordReference,
			    const char *attribute,
			    const char *value,
			    BOOL addValue)
{
	tDirStatus status = eDSNoErr;
	tDataNodePtr attributeType = NULL;
	tDataNodePtr attributeValue = NULL;
	tAttributeValueEntryPtr currentAttributeValueEntry = NULL;
	tAttributeValueEntryPtr newAttributeValueEntry = NULL;

	attributeType = dsDataNodeAllocateString(ods_state->session.ref,
						attribute);
	if (addValue) {
		attributeValue =
		    dsDataNodeAllocateString(ods_state->session.ref, value);
		status = dsAddAttributeValue(recordReference, attributeType,
						attributeValue);
		LOG_DS_ERROR(ds_trace, status, "dsAddAttributeValue");
	} else {

#ifdef USE_SETATTRIBUTEVALUE
		status = dsGetRecordAttributeValueByIndex(recordReference,
					attributeType, 1,
					&currentAttributeValueEntry);
		LOG_DS_ERROR(ds_trace, status,
			    "dsGetRecordAttributeValueByIndex");

		if (eDSNoErr == status) {
			newAttributeValueEntry =
				dsAllocAttributeValueEntry(ods_state->session.ref,
						currentAttributeValueEntry->fAttributeValueID, (char *)value, strlen(value));
			status = dsSetAttributeValue(recordReference, attributeType, newAttributeValueEntry);
			LOG_DS_ERROR(ds_trace, status, "dsSetAttributeValue");

			dsDeallocAttributeValueEntry(ods_state->session.ref, newAttributeValueEntry);
		}

		if (currentAttributeValueEntry) {
			dsDeallocAttributeValueEntry(ods_state->session.ref,
				    currentAttributeValueEntry);
		}

#else /* USE_SETATTRIBUTEVALUE */

		status = dsRemoveAttribute(recordReference, attributeType);
		LOG_DS_ERROR(ds_trace, status, "dsRemoveAttribute");

		attributeValue =
		    dsDataNodeAllocateString(ods_state->session.ref, value);
		status = dsAddAttribute(recordReference, attributeType, 0, attributeValue);
		LOG_DS_ERROR(ds_trace, status, "dsAddAttribute");

#endif /* USE_SETATTRIBUTEVALUE */

	}

	opendirectory_free_node(&ods_state->session, attributeType);
	opendirectory_free_node(&ods_state->session, attributeValue);
	return status;
}

static tDirStatus get_records(struct odssam_privates *ods_state,
			    CFMutableArrayRef recordsArray,
			    tDirNodeReference nodeRef,
			    tDataListPtr recordName,
			    tDataListPtr recordType,
			    tDataListPtr attributes)
{
	tDirStatus		status		=	eDSNoErr;
	tDataBufferPtr		dataBuffer	=	NULL;
	UInt32			recordCount	=	0;
	tContextData		*currentContextData = (tContextData)NULL;

	currentContextData = &ods_state->contextData;
	dataBuffer = DS_DEFAULT_BUFFER(ods_state->session.ref);

	status = dsGetRecordList(nodeRef, dataBuffer,
			    recordName, eDSiExact, recordType,
			    attributes, False, &recordCount,
			    currentContextData);

	LOG_DS_ERROR(ds_trace, status, "dsGetRecordList");
	if (status != eDSNoErr) {
		goto cleanup;
	}

	DEBUG(module_debug,("get_records recordCount (%u)\n", recordCount));
	opendirectory_insert_search_results(nodeRef, recordsArray,
			    recordCount, dataBuffer);

cleanup:
	opendirectory_free_buffer(&ods_state->session, dataBuffer);
	return status;
}

/* Like opendirectory_sam_searchname except that it does a paged search using
 * the context data in ods_state.
 */
static tDirStatus get_sam_record_attributes(
		    struct odssam_privates *ods_state,
		    CFMutableArrayRef recordsArray,
		    const char *type,
		    const char *name)
{
	tDirStatus status;
	tDataListPtr samAttributes = NULL;
	tDataListPtr recordName = NULL;
	tDataListPtr recordType = NULL;

	status = opendirectory_reconnect(&ods_state->session);
	if (status != eDSNoErr) {
		return status;
	}

	status = opendirectory_searchnode(&ods_state->session);
	if (status != eDSNoErr) {
		return status;
	}

	recordName = dsBuildListFromStrings(ods_state->session.ref,
				name ? name : kDSRecordsAll, NULL);

	recordType = dsBuildListFromStrings(ods_state->session.ref,
				type, NULL);
	if (recordName == NULL || recordType == NULL) {
		status = eDSAllocationFailed;
		goto cleanup;
	}

	samAttributes = opendirectory_sam_attrlist(&ods_state->session);
	if (samAttributes == NULL) {
		status = eDSAllocationFailed;
		goto cleanup;
	}

	status = get_records(ods_state, recordsArray,
			ods_state->session.search,
			recordName, recordType,
			samAttributes);
	LOG_DS_ERROR(ds_trace, status, "get_records");

cleanup:
	opendirectory_free_list(&ods_state->session, recordName);
	opendirectory_free_list(&ods_state->session, recordType);
	opendirectory_free_list(&ods_state->session, samAttributes);

	return status;
}

/*******************************************************************
search an attribute and return the first value found.
******************************************************************/
static BOOL get_single_attribute (CFDictionaryRef entry,
				  const char *attr, pstring value)
{
        CFStringRef attrRef = NULL;
        const void  *opaque_value = NULL;
	pstring	    buffer;
        BOOL	    result = False;

	DEBUG(module_debug, ("get_single_attribute attr=%s ", attr));

	attrRef = CFStringCreateWithCString(NULL, attr,
				kCFStringEncodingUTF8);
	if (!CFDictionaryGetValueIfPresent(entry, attrRef, &opaque_value)) {
		goto done;
	}

	if (CFGetTypeID(opaque_value) == CFArrayGetTypeID()) {
		CFArrayRef valueList = (CFArrayRef)opaque_value;
		CFStringRef cfstrRef;

		if (CFArrayGetCount(valueList) == 0) {
			goto done;
		}

		cfstrRef = (CFStringRef)CFArrayGetValueAtIndex(valueList, 0);
		if (cfstrRef == NULL) {
			goto done;
		}

		if (GetCString(cfstrRef, buffer, sizeof(buffer))) {
			pstrcpy(value, buffer);
			result = True;
			goto done;
		}

		DEBUGADD(module_debug, ("CFArrayRef[0] <does not exist>]\n"));

	} else if (CFGetTypeID(opaque_value) == CFStringGetTypeID()) {
		CFStringRef cfstrRef = (CFStringRef)opaque_value;

		if (cfstrRef == NULL) {
			goto done;
		}

		if (GetCString(cfstrRef, buffer, PSTRING_LEN)) {
			pstrcpy(value, buffer);
			result = True;
			goto done;
		}

		DEBUGADD(module_debug, ("CFStringRef <does not exist>\n"));
	}

done:
	DEBUGADD(module_debug, ("%s\n", result ? "OK" : "<does not exist>"));

	if (attrRef)
		CFRelease(attrRef);

#ifdef DEBUG_PASSWORDS
	if (result == True) {
		DEBUG(module_debug, ("get_single_attribute: [%s] = [%s]\n",
			    attr, value));
	}
#endif

	return result;
}

static BOOL get_attributevalue_list (CFDictionaryRef entry,
				  const char *attribute, CFArrayRef *valueList)
{
	CFStringRef attrRef = NULL;
	BOOL result = False;

	attrRef = CFStringCreateWithCString(NULL, attribute, kCFStringEncodingUTF8);
	*valueList = (CFArrayRef)CFDictionaryGetValue(entry, attrRef);

	if (*valueList != NULL && CFArrayGetCount(*valueList)) {
		result = True;
	}

	if (attrRef)
		CFRelease(attrRef);

	return result;
}

static BOOL parse_password_server(CFArrayRef authAuthorities, char *pwsServerIP, char *userid)
{
	BOOL result = False;
	int arrayCount, arrayIndex;
	char *tmp = NULL;
	char *current = NULL;
	const char *pwdsrvr = "PasswordServer";
	const char *delimiter = ";";
	CFStringRef authEntry = NULL;
	char authStr[512];

	for (arrayIndex = 0, arrayCount = CFArrayGetCount(authAuthorities); arrayIndex < arrayCount; arrayIndex++) {
		authEntry = CFArrayGetValueAtIndex(authAuthorities, arrayIndex);
		if (GetCString(authEntry, authStr, 512)) {
			if (strstr(authStr, pwdsrvr) != NULL) {
				//;ApplePasswordServer;0x3e5d410b7be9c1bd0000000200000002:17.221.41.95;
				current = authStr;
				tmp = strsep(&current, delimiter); // tmp = ;
				if (NULL == tmp)
					break;
				tmp = strsep(&current, delimiter); // tmp = ApplePassworServer
				if (NULL == tmp)
					break;
				tmp = strsep(&current, ":"); // tmp = 0xXXXXX
				if (NULL == tmp)
					break;
				safe_strcpy(userid,tmp, 1024);
				DEBUG(module_debug, ("parse_password_server: userid(%s)\n",userid));
				tmp = strsep(&current, ";"); // tmp = xx.xx.xx
				if (NULL == tmp)
					break;
				safe_strcpy(pwsServerIP,tmp, 1024);
				DEBUG(module_debug, ("parse_password_server: pwsServerIP(%s)\n",pwsServerIP));
				result = True;
				break;
			}
		}

	}

	return result;
}
static BOOL insert_passwordpolicy_attributes(CFMutableDictionaryRef entry,
				const char *policy)
{
	BOOL result = False;
	// attr=value attr=value

    char *tmp = NULL;
    const char *delimiter = "=";
    char *current = NULL;
    char *original = NULL;
    CFStringRef key = NULL;
    CFStringRef value = NULL;
	CFMutableArrayRef valueArray = NULL;

    current = SMB_STRDUP(policy);
    original = current;
	do {
		tmp = strsep(&current, delimiter);
		DEBUG(module_debug, ("insert_passwordpolicy_attributes: key(%s)\n",tmp));
		if (tmp != NULL)
			key = CFStringCreateWithCString(NULL, tmp, kCFStringEncodingUTF8);
		else
			key = NULL;

		tmp = strsep(&current, " ");
		DEBUG(module_debug, ("insert_passwordpolicy_attributes: value(%s)\n",tmp));
		if (tmp != NULL)
			value = CFStringCreateWithCString(NULL, tmp, kCFStringEncodingUTF8);
		else
			value = NULL;

		if (key && value) {
			valueArray = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
			CFArrayAppendValue(valueArray, value);
			CFDictionaryAddValue(entry, key, valueArray);
			CFRelease(valueArray);
			result = true;
		}
		if (key)
			CFRelease(key);
		if (value)
			CFRelease(value);

	} while (current != NULL);
	free(original);
	return result;
}

static tDirStatus get_passwordpolicy_attributes(
		struct odssam_privates *ods_state,
		struct samu * sampass,
		CFDictionaryRef entry,
		CFMutableDictionaryRef *ppentry)
{
	tDirStatus status = eDSNoErr;
	char pwsServerIP[1024];
	char userid[1024];
	CFArrayRef authAuthorities = NULL;
	const char *recordType = NULL;
	char dirNode[512] = {0};
	tDirNodeReference nodeReference = 0;
	tRecordReference recordReference = 0;
	char policy[1024];
	const char * userName;

	userName = pdb_get_username(sampass);
	SMB_ASSERT(userName != NULL);

	if (!get_attributevalue_list(entry, kDSNAttrAuthenticationAuthority,
				&authAuthorities)) {
		goto cleanup;
	}

	if (!parse_password_server(authAuthorities, pwsServerIP, userid)) {
		goto cleanup;
	}

	if (!get_single_attribute(entry, kDSNAttrMetaNodeLocation, dirNode)) {
		status =  eDSInvalidNodeRef;
	} else {
		status = opendirectory_open_node(&ods_state->session, dirNode, &nodeReference);
		if (eDSNoErr != status){
			goto cleanup;
		}

#if AUTH_GET_POLICY
		status = opendirectory_authenticate_node(ods_state->session.ref,
						    nodeReference);
		if (eDSNoErr != status) {
			goto cleanup;
		}
#endif

		if (eDSNoErr == status) {
			recordType = get_record_type((const char *)userName);
			status = get_record_ref(ods_state, nodeReference,
					&recordReference, recordType, userName);
		}
	}

	if (eDSNoErr == status && nodeReference != 0) {

		status = get_password_policy(ods_state, nodeReference,
				userName, policy, recordType);
		DEBUG(module_debug, ("%s: [%d]get_password_policy (%s, %s)\n",
			MODULE_NAME, status, policy, recordType));

		/* If we found some password policy attributes, clone the
		 * record and insert attributes corresponding to the password
		 * policy.
		 */
		if (eDSNoErr == status) {
			*ppentry = CFDictionaryCreateMutableCopy(NULL, 0,
								    entry);

			insert_passwordpolicy_attributes(*ppentry, policy);
		}
	}

cleanup:
	DS_CLOSE_NODE(nodeReference);

	return status;
}

tDirStatus add_record_attributes(struct odssam_privates *ods_state, CFDictionaryRef samAttributes, CFDictionaryRef userCurrent, const char *the_record_type)
{
	tDirStatus status = eDSNoErr;

	int attributeIndex;
	int count = CFDictionaryGetCount(samAttributes);
	CFStringRef values[count];
	CFStringRef keys[count];
	char key[255] = {0};
	char value[255] = {0};
	Boolean isKey,isValue;
	char dirNode[512] = {0};
	char temp[512] = {0};
	char userName[128] = {0};
	char policy[1024] = {0};
	BOOL addValue;

	tDirNodeReference nodeReference = 0;
	tRecordReference recordReference = 0;
	const char *recordType = NULL;

	if (!get_single_attribute(userCurrent, kDSNAttrRecordName, userName)) {
		status = eDSNullParameter;
		goto cleanup;
	}
	if (!get_single_attribute(userCurrent, kDSNAttrMetaNodeLocation, dirNode)) {
		status =  eDSInvalidNodeRef;
	} else {
		status = opendirectory_open_node(&ods_state->session, dirNode, &nodeReference);
		LOG_DS_ERROR(ds_trace, status, "opendirectory_open_node");
		if (eDSNoErr == status) {
			status = opendirectory_authenticate_node(
						    &ods_state->session,
						    nodeReference);
			if (eDSNoErr == status) {
				if (the_record_type)
					recordType = the_record_type;
				else
					recordType = get_record_type((const char *)userName);
				status = get_record_ref(ods_state, nodeReference, &recordReference, recordType, userName);
			}
		} else {
			goto cleanup;
		}
	}

	if (eDSNoErr == status) {
		for (attributeIndex = 0, CFDictionaryGetKeysAndValues(samAttributes, (const void**)keys, (const void**)values); attributeIndex < count; attributeIndex++) {
			isKey = CFStringGetCString(keys[attributeIndex], key, sizeof(key), kCFStringEncodingUTF8);
			isValue = CFStringGetCString(values[attributeIndex], value, sizeof(value), kCFStringEncodingUTF8);

			if (get_single_attribute(userCurrent,key, temp))
				addValue = false;
			else
				addValue = true;

			if (isKey && isValue) {
				if (strcmp(key, kDSStdAuthSetPasswdAsRoot) == 0
#if defined(kDSStdAuthSetWorkstationPasswd) && defined(kDSStdAuthSetLMHash)
				|| strcmp(key, kDSStdAuthSetWorkstationPasswd) == 0 || strcmp(key, kDSStdAuthSetLMHash) == 0
#endif
				) {
					status = set_password(ods_state, nodeReference, userName, value, key, recordType);
				#ifdef DEBUG_PASSWORDS
					DEBUG (module_debug, ("add_record_attributes: [%d]SetPassword(%s, %s, %s, %s)\n",status, userName, key, value, recordType));
				else
					DEBUG (module_debug, ("add_record_attributes: [%d]SetPassword(%s, %s, %s)\n",status, userName, key, recordType));
				#endif
				} else if (strcmp(key, kDSNAttrRecordName) == 0) {

					if (strcmp(userName, value) != 0) {
						status = set_recordname(ods_state, recordReference, value);
						DEBUG (module_debug, ("add_record_attributes: [%d]set_recordname(%s, %s, %s)\n",status, userName, key, value));
					}
				} else if (isPWSAttribute(key)) {
					add_password_policy_attribute(policy, key, value);
				} else {
					status = add_attribute_with_value(ods_state, recordReference, key, value, addValue);
					DEBUG (module_debug, ("[%d]add_record_attributes: add_attribute_with_value(%s,%s,%s) error\n",status, userName, key, value));
				}
				if (status != eDSNoErr)
					break;
			}
		}
		if (strlen(policy) > 0)
			status =  set_password_policy(ods_state, nodeReference, userName, policy, recordType);
	} else {
		DEBUG (module_debug, ("[%d]add_record_attributes: authenticate_node error\n",status));
	}
cleanup:
	if (0 != recordReference)
		dsCloseRecord(recordReference);
	DS_CLOSE_NODE(nodeReference);
	return status;
}

/**********************************************************************
Initialize struct samu from an Open Directory query
(Based on init_sam_from_buffer in pdb_tdb.c)
*********************************************************************/
static BOOL init_sam_from_ods (struct odssam_privates *ods_state,
				struct samu * sampass,
				CFDictionaryRef entry)
{
	CFMutableDictionaryRef ppentry = NULL;
	time_t  logon_time,
			logoff_time,
			kickoff_time,
			pass_last_set_time,
			pass_can_change_time,
			pass_must_change_time;
	pstring 	username,
			nt_username,
			fullname,
			homedir,
			dir_drive,
			logon_script,
			profile_path,
			acct_desc,
			munged_dial,
			workstations;
	uint32	is_disabled;
#if WITH_PASSWORD_HASH
	uint8 	smblmpwd[LM_HASH_LEN],
			smbntpwd[NT_HASH_LEN];
#endif
	uint16 		acct_ctrl = 0,
			logon_divs;
	uint32 		hours_len;
	uint8 		hours[MAX_HOURS_LEN];
	pstring 	temp;
	pstring 	boolFlag;
	uid_t		uid = guest_uid;
	gid_t 		gid = getegid();
	DOM_SID		sid;

	/*
	 * do a little initialization
	 */
	username[0] 	= '\0';
	nt_username[0] 	= '\0';
	fullname[0] 	= '\0';
	homedir[0] 	= '\0';
	dir_drive[0] 	= '\0';
	logon_script[0] = '\0';
	profile_path[0] = '\0';
	acct_desc[0] 	= '\0';
	munged_dial[0] 	= '\0';
	workstations[0] = '\0';


	SMB_ASSERT(sampass != NULL);
	SMB_ASSERT(ods_state != NULL);
	SMB_ASSERT(entry != NULL);

	get_single_attribute(entry, kDSNAttrRecordName, username);
	DEBUG(module_debug, ("Entry found for user: %s\n", username));

	get_single_attribute(entry, kDS1AttrDistinguishedName, nt_username);
	pdb_set_username(sampass, username, PDB_SET);

	pdb_set_domain(sampass, lp_workgroup(), PDB_DEFAULT);
	pdb_set_nt_username(sampass, nt_username, PDB_SET);

	get_passwordpolicy_attributes(ods_state, sampass, entry, &ppentry);
	if (ppentry) {
		entry = (CFDictionaryRef)ppentry;
	}

	if (get_single_attribute(entry, kDS1AttrUniqueID, temp)) {
		uid = atol(temp);
	}

	if (opendirectory_find_usersid_from_record(&ods_state->session,
					entry, &sid)) {
		pdb_set_user_sid(sampass, &sid, PDB_SET);
	}

	if (opendirectory_find_groupsid_from_record(&ods_state->session,
					entry, &sid)) {
		/* Don't call pdb_set_group_sid here, since it does a SID to
		 * GID resolution that can call back into us and blow the
		 * stack.
		 */
		if (!sampass->group_sid) {
			sampass->group_sid = TALLOC_P(sampass, DOM_SID);
		}

		if (sampass->group_sid) {
			sid_copy(sampass->group_sid, &sid);
			pdb_set_init_flags(sampass, PDB_GROUPSID, PDB_SET);
		}
	}

	if (get_single_attribute(entry, kDS1AttrPrimaryGroupID, temp)) {
		gid = atol(temp);
	}

	if (get_single_attribute(entry, kDS1AttrNFSHomeDirectory, temp)) {
                pdb_set_homedir(sampass, temp, PDB_SET);
	}

#if 0
	if (group_rid == 0) {
		GROUP_MAP map;

		get_single_attribute(entry, kDS1AttrPrimaryGroupID, temp);
		gid = atol(temp);
		/* call the mapping code here */
		if(pdb_getgrgid(&map, gid)) {
			pdb_set_group_sid(sampass, &map.sid, PDB_SET);
		}
		else {
			group_rid = pdb_gid_to_group_rid(gid);
		if(odsam_get_record_sid(ods_state, sampass, entry, group_rid, temp))
			pdb_set_group_sid_from_string(sampass, temp, PDB_SET);
		}
	}
#endif

	if (get_single_attribute(entry, kPWSPWDLastSetTime, temp)) {
		pass_last_set_time = (time_t) atol(temp);
		pdb_set_pass_last_set_time(sampass, pass_last_set_time, PDB_SET);
	}

	if (get_single_attribute(entry, kPWSLastLoginTime, temp)) {
		logon_time = (time_t) atol(temp);
		pdb_set_logon_time(sampass, logon_time, PDB_SET);
	}

	if (get_single_attribute(entry, kPWSLogOffTime, temp)) {
		logoff_time = (time_t) atol(temp);
		pdb_set_logoff_time(sampass, logoff_time, PDB_SET);
	}

	if (get_single_attribute(entry, kPWSKickOffTime, temp)) {
		kickoff_time = (time_t) atol(temp);
		pdb_set_kickoff_time(sampass, kickoff_time, PDB_SET);
	}

	if (get_single_attribute(entry, kPWSExpiryDate, temp)) {
		pass_can_change_time = (time_t) atol(temp);
		pdb_set_pass_can_change_time(sampass, pass_can_change_time, PDB_SET);
	}

	if (get_single_attribute(entry, kPWSHardExpiryDate, temp)) {
		if (get_single_attribute(entry, kPWSExpiryDateEnabled, boolFlag) && strcmp(boolFlag,"1") == 0) {
			if (get_single_attribute(entry, kPWSNewPasswordRequired, boolFlag) && strcmp(boolFlag,"1") == 0) {
				struct timeval tv;
				GetTimeOfDay(&tv);
				pass_must_change_time = (time_t) tv.tv_sec;
			} else
				pass_must_change_time = (time_t) atol(temp);
			pdb_set_pass_must_change_time(sampass, pass_must_change_time, PDB_SET);
		}
	}

	if (get_single_attribute(entry, kDS1AttrDistinguishedName, fullname)) {
		pdb_set_fullname(sampass, fullname, PDB_SET);
	}

	if (!get_single_attribute(entry, kDS1AttrSMBHomeDrive, dir_drive)) {
#ifdef GLOBAL_DEFAULT
		pdb_set_dir_drive(sampass,
			talloc_sub_specified(ods_state, lp_logon_drive(),
					  username, pdb_get_domain(sampass),
					  uid, gid),
			PDB_DEFAULT);
#else
		pdb_set_dir_drive(sampass, NULL, PDB_SET);
#endif
	} else {
		pdb_set_dir_drive(sampass, dir_drive, PDB_SET);
	}

	if (!get_single_attribute(entry, kDS1AttrSMBHome, homedir)) {
#ifdef GLOBAL_DEFAULT
		pdb_set_homedir(sampass,
			talloc_sub_specified(ods_state, lp_logon_home(),
					  username, pdb_get_domain(sampass),
					  uid, gid),
		PDB_DEFAULT);
#else
		pdb_set_homedir(sampass, NULL, PDB_SET);
#endif
	} else {
		pdb_set_homedir(sampass, homedir, PDB_SET);
	}

	if (!get_single_attribute(entry, kDS1AttrSMBScriptPath, logon_script)) {
#ifdef GLOBAL_DEFAULT
		pdb_set_logon_script(sampass,
			talloc_sub_specified(ods_state, lp_logon_script(),
				 username, pdb_get_domain(sampass), uid, gid),
			PDB_DEFAULT);
#else
		pdb_set_logon_script(sampass, NULL, PDB_SET);
#endif
	} else {
		pdb_set_logon_script(sampass, logon_script, PDB_SET);
	}

	if (!get_single_attribute(entry, kDS1AttrSMBProfilePath, profile_path)) {
#ifdef GLOBAL_DEFAULT
		pdb_set_profile_path(sampass,
			talloc_sub_specified(ods_state, lp_logon_path(),
				 username, pdb_get_domain(sampass), uid, gid),
			PDB_DEFAULT);
#else
		pdb_set_profile_path(sampass, NULL, PDB_SET);
#endif
	} else {
		pdb_set_profile_path(sampass, profile_path, PDB_SET);
	}

	if (get_single_attribute(entry, kDS1AttrComment, acct_desc)) {
		pdb_set_acct_desc(sampass, acct_desc, PDB_SET);
	}

	if (get_single_attribute(entry, kDS1AttrSMBUserWorkstations,
				    workstations)) {
		pdb_set_workstations(sampass, workstations, PDB_SET);
	}

	/* FIXME: hours stuff should be cleaner */

	logon_divs = 168;
	hours_len = 21;
	memset(hours, 0xff, hours_len);
#if WITH_PASSWORD_HASH
	if (get_single_attribute (entry, kDS1AttrSMBLMPassword, temp)) {
		pdb_gethexpwd(temp, smblmpwd);
		memset((char *)temp, '\0', strlen(temp)+1);
		pdb_set_lanman_passwd(sampass, smblmpwd, PDB_SET);
		ZERO_STRUCT(smblmpwd);
	}

	if (get_single_attribute (entry, kDS1AttrSMBNTPassword, temp)) {
		pdb_gethexpwd(temp, smbntpwd);
		memset((char *)temp, '\0', strlen(temp)+1);
		pdb_set_nt_passwd(sampass, smbntpwd, PDB_SET);
		ZERO_STRUCT(smbntpwd);
	}
#endif
	if (!get_single_attribute (entry, kDS1AttrSMBAcctFlags, temp)) {
		acct_ctrl |= ACB_NORMAL;
	} else {
		acct_ctrl = pdb_decode_acct_ctrl(temp);

		if (acct_ctrl == 0)
			acct_ctrl |= ACB_NORMAL;

		if (get_single_attribute (entry, kPWSIsDisabled, temp)) {
			is_disabled = atol(temp);
			if (is_disabled == 1)
				acct_ctrl |= ACB_DISABLED;
		}

		pdb_set_acct_ctrl(sampass, acct_ctrl, PDB_SET);
	}

	pdb_set_hours_len(sampass, hours_len, PDB_SET);
	pdb_set_logon_divs(sampass, logon_divs, PDB_SET);

	pdb_set_munged_dial(sampass, munged_dial, PDB_SET);

	/* pdb_set_unknown_3(sampass, unknown3, PDB_SET); */
	/* pdb_set_unknown_5(sampass, unknown5, PDB_SET); */
	/* pdb_set_unknown_6(sampass, unknown6, PDB_SET); */

	pdb_set_hours(sampass, hours, PDB_SET);


	if (ppentry) {
		CFRelease(ppentry);
	}
	return True;
}

static void update_user_entry(CFMutableDictionaryRef userEntry,
	const char *attribute,
	const char *value)
{
	static const char const func[] = "update_user_entry";
	CFStringRef cfKey = NULL;
	CFStringRef cfValue = NULL;

	if (attribute && *attribute) {
		cfKey = CFStringCreateWithCString(NULL, attribute, kCFStringEncodingUTF8);
	} else {
		DEBUG(module_debug, ("%s: missing attribute!!!\n", func));
	}

	if (value && *value) {
		cfValue = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
	} else {
		DEBUG(module_debug, ("%s: missing value!!!\n", func));
	}

	if (cfKey && cfValue) {
		CFDictionaryAddValue(userEntry, cfKey, cfValue);
	}

	if(cfKey) {
		CFRelease(cfKey);
	}

	if(cfValue) {
		CFRelease(cfValue);
	}
}

static BOOL need_ods_mod(BOOL pdb_add,
	const struct samu * sampass,
	enum pdb_elements element)
{
	if (pdb_add) {
		return (!IS_SAM_DEFAULT(sampass, element));
	} else {
		return IS_SAM_CHANGED(sampass, element);
	}
}

/**********************************************************************
Initialize Open Directory account from struct samu
*********************************************************************/

static BOOL init_ods_from_sam(struct odssam_privates *ods_state,
	BOOL pdb_add,
	struct samu * sampass,
	CFMutableDictionaryRef userEntry)
{
	static const char const func[] = "init_ods_from_sam";

	pstring temp;
	uint32 rid;

	SMB_ASSERT(ods_state != NULL);
	SMB_ASSERT(sampass != NULL);

	if (need_ods_mod(pdb_add, sampass, PDB_USERNAME)) {
		update_user_entry(userEntry, kDSNAttrRecordName,
			pdb_get_username(sampass));
		DEBUG(module_debug, ("Setting entry for user: %s\n",
			    pdb_get_username(sampass)));
	}

	if (need_ods_mod(pdb_add, sampass, PDB_USERSID)) {
		fstring sid_str;
		fstring dom_sid_str;
		const DOM_SID *user_sid = pdb_get_user_sid(sampass);

		if (NULL != user_sid) {
			if (!sid_check_is_in_our_domain(user_sid)) {
				DEBUG(module_debug, ("%s: User's SID (%s) is not for "
				    "this domain (%s), cannot add to "
				    "Open Directory!\n",
					func, sid_to_string(sid_str, user_sid),
					sid_to_string(dom_sid_str, get_global_sam_sid())));
				return False;
			}
			sid_to_string(sid_str, user_sid);
			DEBUG(module_debug, ("Setting SID entry for user: %s [%s]\n",
				    pdb_get_username(sampass), sid_str));
			update_user_entry(userEntry, kDS1AttrSMBSID, sid_str);
		} else if ((rid = pdb_get_user_rid(sampass))!=0) {
			slprintf(temp, sizeof(temp) - 1, "%i", rid);
			DEBUG(module_debug, ("Setting RID entry for user: %s [%s]\n",
				    pdb_get_username(sampass), temp));
			update_user_entry(userEntry, kDS1AttrSMBRID, temp);
		}
#ifdef STORE_ALGORITHMIC_RID
		 else if (!IS_SAM_DEFAULT(sampass, PDB_UID)) {
			rid = algorithmic_pdb_uid_to_user_rid(pdb_get_uid(sampass));
			slprintf(temp, sizeof(temp) - 1, "%i", rid);
			update_user_entry(userEntry, kDS1AttrSMBRID, temp);
		} else {
			DEBUG(module_debug, ("NO user RID specified on account %s, cannot store!\n", pdb_get_username(sampass)));
			return False;
		}
#endif
	}

	if (need_ods_mod(pdb_add, sampass, PDB_GROUPSID)) {
		fstring sid_str;
		fstring dom_sid_str;
		const DOM_SID *group_sid = pdb_get_group_sid(sampass);


		if (NULL != group_sid) {
			if (!sid_check_is_in_our_domain(group_sid)) {
				DEBUG(module_debug, ("init_ods_from_sam: User's Primary Group SID (%s) is not for this domain (%s), cannot add to Open Directory!\n",
					sid_to_string(sid_str, group_sid),
					sid_to_string(dom_sid_str, get_global_sam_sid())));
				return False;
			}
			sid_to_string(sid_str, group_sid);
			DEBUG(module_debug, ("Setting Primary Group SID entry "
				   "for user: %s [%s]\n",
			       pdb_get_username(sampass), sid_str));
			update_user_entry(userEntry,
				kDS1AttrSMBPrimaryGroupSID, sid_str);
		} else if ((rid = pdb_get_group_rid(sampass))!=0) {
			slprintf(temp, sizeof(temp) - 1, "%i", rid);
			update_user_entry(userEntry, kDS1AttrSMBGroupRID, temp);
		}
#ifdef STORE_ALGORITHMIC_RID
		else if (!IS_SAM_DEFAULT(sampass, PDB_GID)) {
			rid = pdb_gid_to_group_rid(pdb_get_gid(sampass));
			slprintf(temp, sizeof(temp) - 1, "%i", rid);
			update_user_entry(userEntry, kDS1AttrSMBGroupRID, temp);
		} else {
			DEBUG(module_debug, ("NO group RID specified on account %s, cannot store!\n", pdb_get_username(sampass)));
			return False;
		}
#endif
	}

	/* displayName, cn, and gecos should all be the same
	 *  most easily accomplished by giving them the same OID
	 *  gecos isn't set here b/c it should be handled by the
	 *  add-user script
	 */
	if (need_ods_mod(pdb_add, sampass, PDB_FULLNAME)) {
		update_user_entry(userEntry, kDS1AttrDistinguishedName,
			pdb_get_fullname(sampass));
	}
	if (need_ods_mod(pdb_add, sampass, PDB_ACCTDESC)) {
		update_user_entry(userEntry, kDS1AttrComment,
			pdb_get_acct_desc(sampass));
	}
	if (need_ods_mod(pdb_add, sampass, PDB_WORKSTATIONS)) {
		update_user_entry(userEntry,  kDS1AttrSMBUserWorkstations,
			pdb_get_workstations(sampass));
	}
	/*
	 * Only updates fields which have been set (not defaults from smb.conf)
	 */

	if (need_ods_mod(pdb_add, sampass, PDB_SMBHOME)) {
		update_user_entry(userEntry, kDS1AttrSMBHome,
			pdb_get_homedir(sampass));
	}

	if (need_ods_mod(pdb_add, sampass, PDB_DRIVE)) {
		update_user_entry(userEntry, kDS1AttrSMBHomeDrive,
			pdb_get_dir_drive(sampass));
	}

	if (need_ods_mod(pdb_add, sampass, PDB_LOGONSCRIPT)) {
		update_user_entry(userEntry, kDS1AttrSMBScriptPath,
			pdb_get_logon_script(sampass));
	}

	if (need_ods_mod(pdb_add, sampass, PDB_PROFILE))
		update_user_entry(userEntry, kDS1AttrSMBProfilePath,
			pdb_get_profile_path(sampass));

	if (need_ods_mod(pdb_add, sampass, PDB_LOGONTIME)) {
		slprintf(temp, sizeof(temp) - 1, "%li",
			pdb_get_logon_time(sampass));
		update_user_entry(userEntry, kPWSLastLoginTime, temp);
	}

	if (need_ods_mod(pdb_add, sampass, PDB_LOGOFFTIME)) {
		slprintf(temp, sizeof(temp) - 1, "%li",
			pdb_get_logoff_time(sampass));
		update_user_entry(userEntry, kPWSLogOffTime, temp);
	}

	if (need_ods_mod(pdb_add, sampass, PDB_KICKOFFTIME)) {
		slprintf (temp, sizeof (temp) - 1, "%li",
			pdb_get_kickoff_time(sampass));
		update_user_entry(userEntry, kPWSKickOffTime, temp);
	}


	if (need_ods_mod(pdb_add, sampass, PDB_CANCHANGETIME)) {
		slprintf (temp, sizeof (temp) - 1, "%li",
			pdb_get_pass_can_change_time(sampass));
		update_user_entry(userEntry, kPWSExpiryDate, temp);
	}

	if (need_ods_mod(pdb_add, sampass, PDB_MUSTCHANGETIME)) {
		slprintf (temp, sizeof (temp) - 1, "%li",
			pdb_get_pass_must_change_time(sampass));
		update_user_entry(userEntry, kPWSHardExpiryDate, temp);
	}

	if (pdb_get_acct_ctrl(sampass) &
		    (ACB_WSTRUST|ACB_SVRTRUST|ACB_DOMTRUST|ACB_NORMAL)) {

#if defined(kDSStdAuthSetWorkstationPasswd) && defined(kDSStdAuthSetLMHash)
		if (need_ods_mod(pdb_add, sampass, PDB_LMPASSWD)) {
			pdb_sethexpwd (temp, pdb_get_lanman_passwd(sampass),
				pdb_get_acct_ctrl(sampass));
			update_user_entry (userEntry, kDSStdAuthSetLMHash,
				temp);
		}

		if (need_ods_mod(pdb_add, sampass, PDB_NTPASSWD)) {
			pdb_sethexpwd(temp, pdb_get_nt_passwd(sampass),
				pdb_get_acct_ctrl(sampass));
			update_user_entry(userEntry,
				kDSStdAuthSetWorkstationPasswd, temp);
		}
#endif
		if (need_ods_mod(pdb_add, sampass, PDB_PLAINTEXT_PW)) {
			update_user_entry(userEntry, kDSStdAuthSetPasswdAsRoot,
				pdb_get_plaintext_passwd(sampass));
		}
#if USES_PWS
		if (need_ods_mod(pdb_add, sampass, PDB_PASSLASTSET)) {
			slprintf (temp, sizeof (temp) - 1, "%li",
				pdb_get_pass_last_set_time(sampass));
			update_user_entry(userEntry, kPWSPWDLastSetTime, temp);
		}
#endif
	}

	/* FIXME: Hours stuff goes in directory  */
	if (need_ods_mod(pdb_add, sampass, PDB_ACCTCTRL)) {
		char * ctrl = pdb_encode_acct_ctrl(pdb_get_acct_ctrl(sampass),
					NEW_PW_FORMAT_SPACE_PADDED_LEN);
		update_user_entry (userEntry, kDS1AttrSMBAcctFlags,
			ctrl);
	}

	return True;
}

/* passdb functions */


/**********************************************************************
End enumeration of the Open Directory Service password list
*********************************************************************/
static void odssam_endsampwent(struct pdb_methods *methods)
{
	struct odssam_privates *ods_state = get_ods_state(methods);

	SMB_ASSERT(ods_state != NULL);

	CFArrayRemoveAllValues(ods_state->usersArray);
	ods_state->usersIndex = 0;
}

static NTSTATUS odssam_setsampwent(struct pdb_methods *methods,
		BOOL update, uint32 acb_mask)
{
	struct odssam_privates *ods_state = get_ods_state(methods);

	DEBUG(module_debug,("odssam_setsampwent: update(%d)\n", update));

	SMB_ASSERT(ods_state != NULL);
	CFArrayRemoveAllValues(ods_state->usersArray);
	ods_state->usersIndex = 0;

	return NT_STATUS_OK;
}

/**********************************************************************
Get the next entry in the Open Directory Service password database
*********************************************************************/
static NTSTATUS odssam_getsampwent(struct pdb_methods *methods, struct samu *user)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	tDirStatus dirStatus = eDSNoErr;
	int entriesAvailable = 0;
	CFMutableDictionaryRef entry = NULL;

	struct odssam_privates *ods_state = get_ods_state(methods);

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	if (ods_state->usersArray != NULL)
		entriesAvailable = CFArrayGetCount(ods_state->usersArray);
	else
		return 	NT_STATUS_UNSUCCESSFUL; // allocate array???

	if (entriesAvailable == 0 || ods_state->usersIndex >= entriesAvailable) {
		DEBUG(module_debug,("odssam_getsampwent: entriesAvailable(%d) contextData(%p)\n", entriesAvailable, ods_state->contextData));
		CFArrayRemoveAllValues(ods_state->usersArray);

		if (entriesAvailable && ods_state->usersIndex >= entriesAvailable && ods_state->contextData == (tContextData)NULL) {
			odssam_endsampwent(methods);
			return NT_STATUS_UNSUCCESSFUL;
		}

		/* NOTE: we can't use opendirectory_sam_searchname here,
		 * because we want to page through the (potentially large
		 * set of) results.
		 */

		if ((dirStatus = get_sam_record_attributes(ods_state, ods_state->usersArray, kDSStdRecordTypeUsers, NULL)) != eDSNoErr) {
			ret = NT_STATUS_UNSUCCESSFUL;
		} else {
			entriesAvailable = CFArrayGetCount(ods_state->usersArray);
			DEBUG(module_debug,("odssam_getsampwent: entriesAvailable Take 2(%d) contextData(%p)\n", entriesAvailable, ods_state->contextData));
			ods_state->usersIndex = 0;
		}
	}

	if (dirStatus == eDSNoErr && entriesAvailable) {
		entry = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(ods_state->usersArray, ods_state->usersIndex);
		ods_state->usersIndex++;
		if (!init_sam_from_ods(ods_state, user, entry)) {
			DEBUG(module_debug,("odssam_getsampwent: init_sam_from_ods failed for user index(%d)\n", ods_state->usersIndex));
//			CFRelease(entry);
			ret = NT_STATUS_UNSUCCESSFUL;
		}
//        CFRelease(entry);
		ret = NT_STATUS_OK;
	}


	return ret;
}

static NTSTATUS odssam_getsampwnam(struct pdb_methods *methods,
				struct samu *user, const char *sname)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	tDirStatus dirStatus = eDSNoErr;
	const char *recordType = NULL;
	CFMutableDictionaryRef entry = NULL;

	CFMutableArrayRef usersArray = NULL;

	struct odssam_privates *ods_state = get_ods_state(methods);

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	recordType = get_record_type(sname);
	if (!recordType) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	/* Map the user account "guest" to whatever the configured guest
	 * account is. This makes "guest" an alias for the guest account,
	 * which is generally OK since smbfs uses "guest" as the guest
	 * account name.
	 */
	if (strcmp(recordType, kDSStdRecordTypeUsers) == 0 &&
	    lp_parm_bool(GLOBAL_SECTION_SNUM, MODULE_NAME,
				"map guest to guest", True) &&
	    strequal(sname, "guest")) {
		sname = lp_guestaccount();
	}

	dirStatus = opendirectory_sam_searchname(&ods_state->session,
			    &usersArray, recordType, sname);

	if (dirStatus != eDSNoErr) {
		LOG_DS_ERROR_MSG(ds_trace, dirStatus,
			"opendirectory_sam_searchname",
			("no %s record for account '%s'\n", recordType, sname));
		/* FIXME: should map from tDirStatus to NTSTATUS -- jpeach */
		goto done;
	}

	/* handle duplicates - currently uses first match in search policy*/
	entry = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(usersArray, 0);
	if (!init_sam_from_ods(ods_state, user, entry)) {
		DEBUG(module_debug,("odssam_getsampwnam: init_sam_from_ods failed for account '%s'!\n", sname));
	} else {
		ret = NT_STATUS_OK;
	}

done:
	if (usersArray) {
		CFRelease(usersArray);
	}

	return ret;
}

static NTSTATUS odssam_getsampwsid(struct pdb_methods *methods,
				struct samu * account,
				const DOM_SID *user_sid)
{

	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct odssam_privates *ods_state = get_ods_state(methods);
	CFDictionaryRef entry = NULL;
	fstring sid_string;

	if (ods_state == NULL) {
		ret = NT_STATUS_INVALID_HANDLE;
		goto cleanup;
	}

	DEBUG(module_debug,("looking up user SID %s\n",
			    sid_to_string(sid_string, user_sid)));

	entry = opendirectory_find_record_from_usersid(&ods_state->session,
						user_sid);
	if (entry == NULL) {
		ret =  NT_STATUS_NO_SUCH_USER;
		goto cleanup;
	}

	if (!init_sam_from_ods(ods_state, account, entry)) {
		DEBUG(module_debug,("odssam_getsampwsid: init_sam_from_ods failed!\n"));
		ret = NT_STATUS_INTERNAL_ERROR;
		goto cleanup;
	}
	ret = NT_STATUS_OK;

cleanup:
	DEBUG(module_debug,("searching user SID %s, result=%s\n",
		    sid_to_string(sid_string, user_sid), nt_errstr(ret)));

	if (entry) {
		CFRelease(entry);
	}

	return ret;
}

static NTSTATUS odssam_add_sam_account(struct pdb_methods *methods, struct samu * newpwd)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	tDirStatus dirStatus = eDSNoErr;
	const char *recordType = NULL;
	struct odssam_privates *ods_state = get_ods_state(methods);
	int 		ops = 0;
    	CFMutableArrayRef usersArray = NULL;
    	CFMutableDictionaryRef userMods = NULL;
	CFDictionaryRef userCurrent	= NULL;

	const char *username = pdb_get_username(newpwd);
	if (!username || !*username) {
		DEBUG(module_debug, ("Cannot add user without a username!\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	recordType = get_record_type((const char *)username);
	if (opendirectory_sam_searchname(&ods_state->session,
			&usersArray, recordType, username) != eDSNoErr) {
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	/* Check for SMB Attributes and bail if already added. */

#if 0 /* skip duplicates for now */
	if (CFArrayGetCount(usersArray) > 1) {
		DEBUG (module_debug, ("More than one user with that uid exists: bailing out!\n"));
		ret =  NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}
#endif
	/* Check if we need to update an existing entry */

	userMods = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!init_ods_from_sam(ods_state, ops, newpwd, userMods)) {
		DEBUG(module_debug, ("odssam_add_sam_account: init_ods_from_sam failed!\n"));
		ret =  NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	if (CFDictionaryGetCount(userMods) == 0) {
		DEBUG(module_debug,("mods is empty: nothing to add for user: %s\n",pdb_get_username(newpwd)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	userCurrent = (CFDictionaryRef) CFArrayGetValueAtIndex(usersArray, 0);
	dirStatus = add_record_attributes(ods_state, userMods, userCurrent, NULL);
	if (eDSNoErr != dirStatus) {
		ret = NT_STATUS_UNSUCCESSFUL;
		DEBUG(module_debug, ("odssam_add_sam_account: [%d]add_record_attributes\n", dirStatus));
	} else {
		ret = NT_STATUS_OK;
	}

cleanup:
	if (usersArray)
		CFRelease(usersArray);
	if (userMods)
		CFRelease(userMods);

	return ret;
}

static NTSTATUS odssam_update_sam_account(struct pdb_methods *methods,
		struct samu * newpwd)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	tDirStatus dirStatus = eDSNoErr;
	const char *recordType = NULL;
	struct odssam_privates *ods_state = get_ods_state(methods);
	int 		ops = 0;
	CFMutableArrayRef usersArray = NULL;
    	CFMutableDictionaryRef userMods = NULL;
	CFDictionaryRef userCurrent	= NULL;

	const char *username = pdb_get_username(newpwd);
	const char *ntusername = pdb_get_nt_username(newpwd);
	const char *searchname = username;

	if (!username || !*username) {
		DEBUG(module_debug, ("Cannot update a user without a username!\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}
	DEBUG(module_debug, ("odssam_update_sam_account: username(%s) ntusername(%s)\n", username, ntusername));

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	if (IS_SAM_CHANGED(newpwd, PDB_USERNAME))
		searchname = ntusername;
	else if (IS_SAM_CHANGED(newpwd, PDB_FULLNAME))
		searchname = username;
	else
		searchname = username;

	if (IS_SAM_CHANGED(newpwd, PDB_USERNAME) && IS_SAM_CHANGED(newpwd, PDB_FULLNAME)) {
		// record name changed - lookup by kDS1AttrSMBSID
		DEBUG(module_debug, ("odssam_update_sam_account: PDB_USERNAME && PDB_FULLNAME MODIFIED \n"));
		return ret;
	} else {
		recordType = get_record_type((const char *)searchname);
		if (opendirectory_sam_searchname(&ods_state->session,
			    &usersArray, recordType, searchname) != eDSNoErr) {
				DEBUG(module_debug, ("odssam_add_sam_account: "
					    "searchname(%s) NOT FOUND\n",
					    searchname));
				ret = NT_STATUS_UNSUCCESSFUL;
				goto cleanup;
		}
	}
// check for SMB Attributes and bail if already added

#if 0 /* skip duplicates for now */
	if (CFArrayGetCount(usersArray) > 1) {
		DEBUG (module_debug, ("odssam_update_sam_account: More than one user with that uid exists: bailing out!\n"));
		ret =  NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}
#endif
	/* Check if we need to update an existing entry */

	userMods = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (!init_ods_from_sam(ods_state, ops, newpwd, userMods)) {
		DEBUG(module_debug, ("odssam_update_sam_account: init_ods_from_sam failed!\n"));
		ret =  NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	if (CFDictionaryGetCount(userMods) == 0) {
		DEBUG(module_debug,("odssam_update_sam_account: mods is empty: nothing to add for user: %s\n",pdb_get_username(newpwd)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	userCurrent = (CFDictionaryRef) CFArrayGetValueAtIndex(usersArray, 0);
	dirStatus = add_record_attributes(ods_state, userMods, userCurrent, NULL);
	if (eDSNoErr != dirStatus) {
		ret = NT_STATUS_UNSUCCESSFUL;
		DEBUG(module_debug, ("odssam_update_sam_account: [%d]add_record_attributes\n", dirStatus));
	} else {
		ret = NT_STATUS_OK;
	}

cleanup:
	if (usersArray)
		CFRelease(usersArray);
	if (userMods)
		CFRelease(userMods);

	return ret;
}

static BOOL init_group_from_ods(struct odssam_privates *ods_state,
				 GROUP_MAP *map,
				 CFDictionaryRef entry)
{
	pstring temp;

	SMB_ASSERT(ods_state != NULL);
	SMB_ASSERT(map != NULL);
	SMB_ASSERT(entry != NULL);

	if (!opendirectory_find_groupsid_from_record(&ods_state->session,
					entry, &map->sid)) {
		DEBUG(module_debug, ("unable to map group SID\n"));
		return False;
	}

	/* We need to set the type to make the idmap pdb calls happy. Since we
	 * know this is a group record, there are only 3 possiblities.
	 */
	if (sid_check_is_in_builtin(&map->sid) ||
	    sid_check_is_in_wellknown_domain(&map->sid)) {
		/* well-known group */
		map->sid_name_use = SID_NAME_WKN_GRP;
	} else if (sid_check_is_in_our_domain(&map->sid)) {
		/* domain/network group */
		map->sid_name_use = SID_NAME_DOM_GRP;
	} else {
		/* local SAM group */
		map->sid_name_use = SID_NAME_ALIAS;
	}

	if (!get_single_attribute(entry, kDS1AttrPrimaryGroupID, temp)) {
		DEBUG(module_debug, ("init_group_from_ods: Mandatory attribute %s not found\n", kDS1AttrPrimaryGroupID));
		return False;
	}

	map->gid = (gid_t)atol(temp);


	if (!get_single_attribute(entry, kDSNAttrMetaNodeLocation, temp)) {
		DEBUG(module_debug, ("init_group_from_ods: Mandatory attribute %s not found\n", kDS1AttrPrimaryGroupID));
		return False;
	}

	if (!get_single_attribute(entry, kDS1AttrDistinguishedName, temp)) {
		DEBUG(module_debug, ("init_group_from_ods: Attribute %s not found\n", kDS1AttrDistinguishedName));
		if(!get_single_attribute(entry, kDSNAttrRecordName, temp)) {
			DEBUG(module_debug, ("init_group_from_ods: Mandatory attribute %s not found\n", kDSNAttrRecordName));
			return False;
		}
	}
	fstrcpy(map->nt_name, temp);

	if (!get_single_attribute(entry, kDS1AttrComment, temp)) {
		temp[0] = '\0';
	}
	fstrcpy(map->comment, temp);

	return True;
}

static void odssam_endgrpwent(struct pdb_methods *methods)
{
	struct odssam_privates *ods_state = get_ods_state(methods);

	CFArrayRemoveAllValues(ods_state->groupsArray);
	ods_state->groupsIndex = 0;
}

static NTSTATUS odssam_setgrpwent(struct pdb_methods *methods, BOOL update)
{
	odssam_endgrpwent(methods);
	DEBUG(module_debug,("odssam_setgrpwent: update(%d)\n", update));

	return NT_STATUS_OK;
}

static NTSTATUS odssam_getgrpwent(struct pdb_methods *methods, GROUP_MAP *map)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	tDirStatus dirStatus = eDSNoErr;
	int entriesAvailable = 0;
	CFMutableDictionaryRef entry = NULL;

	struct odssam_privates *ods_state = get_ods_state(methods);

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	if (ods_state->groupsArray != NULL)
		entriesAvailable = CFArrayGetCount(ods_state->groupsArray);
	else
		return 	NT_STATUS_UNSUCCESSFUL; // allocate array???

	if (entriesAvailable == 0 || ods_state->groupsIndex >= entriesAvailable) {
		DEBUG(module_debug,("odssam_getgrpwent: entriesAvailable(%d) contextData(%p)\n", entriesAvailable, ods_state->contextData));
		CFArrayRemoveAllValues(ods_state->groupsArray);

		if (entriesAvailable && ods_state->groupsIndex >= entriesAvailable && ods_state->contextData == (tContextData)NULL) {
			odssam_endsampwent(methods);
			return NT_STATUS_UNSUCCESSFUL;
		}

		if ((dirStatus = get_sam_record_attributes(ods_state, ods_state->groupsArray, kDSStdRecordTypeGroups, NULL)) != eDSNoErr) {
			ret = NT_STATUS_UNSUCCESSFUL;
		} else {
			entriesAvailable = CFArrayGetCount(ods_state->groupsArray);
			DEBUG(module_debug,("odssam_getgrpwent: entriesAvailable Take 2(%d) contextData(%p)\n", entriesAvailable, ods_state->contextData));
			ods_state->groupsIndex = 0;
		}
	}

	if (dirStatus == eDSNoErr && entriesAvailable) {
		entry = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(ods_state->groupsArray, ods_state->groupsIndex);
		ods_state->groupsIndex++;
		if (!init_group_from_ods(ods_state, map, entry)) {
			DEBUG(module_debug,("odssam_getgrpwent: init_group_from_ods failed for group index(%d)\n", ods_state->groupsIndex));
			ret = NT_STATUS_UNSUCCESSFUL;
		}
		ret = NT_STATUS_OK;
	}


	return ret;
}

static NTSTATUS odssam_getgrsid(struct pdb_methods *methods,
				GROUP_MAP *map, DOM_SID group_sid)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct odssam_privates *ods_state = get_ods_state(methods);
	CFDictionaryRef entry = NULL;
	fstring sid_string;

	if (ods_state == NULL) {
		ret = NT_STATUS_INVALID_HANDLE;
		goto cleanup;
	}

	DEBUG(module_debug,("looking up group SID %s\n",
			    sid_to_string(sid_string, &group_sid)));

	entry = opendirectory_find_record_from_groupsid(&ods_state->session,
							&group_sid);
	if (!entry) {
		ret = NT_STATUS_NO_SUCH_GROUP;
		goto cleanup;
	}

	if (!init_group_from_ods(ods_state, map, entry)) {
		DEBUG(module_debug,("odssam_getgrsid: init_group_from_ods failed!\n"));
		ret = NT_STATUS_INTERNAL_ERROR;
		goto cleanup;
	}
	ret = NT_STATUS_OK;

cleanup:
	DEBUG(module_debug,("searching group SID %s, result=%s\n",
		    sid_to_string(sid_string, &group_sid), nt_errstr(ret)));

	if (entry) {
		CFRelease(entry);
	}

	return ret;
}

static NTSTATUS odssam_getgrgid(struct pdb_methods *methods,
			GROUP_MAP *map, gid_t gid)
{
	NTSTATUS ret = NT_STATUS_NO_SUCH_GROUP;
	struct odssam_privates *ods_state = get_ods_state(methods);
    	CFMutableArrayRef usersArray = NULL;
	CFMutableDictionaryRef entry = NULL;
	pstring gid_string;

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	ZERO_STRUCTP(map);

	snprintf(gid_string, sizeof(gid_string) - 1, "%i", gid);
	DEBUG(module_debug,("odssam_getgrgid: gid [%s]\n", gid_string));

	if (opendirectory_sam_searchattr(&ods_state->session,
			    &usersArray, kDSStdRecordTypeGroups,
			    kDS1AttrPrimaryGroupID, gid_string) != eDSNoErr) {
		goto cleanup;
	}

	entry = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(usersArray, 0);
	if (!entry) {
		goto cleanup;
	}

	if (!init_group_from_ods(ods_state, map, entry)) {
		DEBUG(module_debug,("odssam_getgrgid: init_group_from_ods failed!\n"));
		goto cleanup;
	}

	/* Fail if we were't able to map the SID for this group. */
	if (!is_null_sid(&map->sid)) {
		ret = NT_STATUS_OK;
	}

cleanup:
	if (usersArray) {
		CFRelease(usersArray);
	}
	return ret;
}

static NTSTATUS odssam_getgrnam(struct pdb_methods *methods,
			GROUP_MAP *map,
			const char *name)
{
	tDirStatus dirStatus = eDSNoErr;
	CFMutableDictionaryRef entry = NULL;
	CFMutableArrayRef recordsArray;

	struct odssam_privates *ods_state = get_ods_state(methods);

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	DEBUG(module_debug,("getting kDSStdRecordTypeGroups and "
		    "kDSNAttrRecordName for <%s>\n", name));

	dirStatus = opendirectory_sam_searchattr(&ods_state->session,
			    &recordsArray, kDSStdRecordTypeGroups,
			    kDSNAttrRecordName, name);

	if (dirStatus != eDSNoErr) {
		DEBUG(module_debug,("getting kDSStdRecordTypeGroups and "
			    "kDS1AttrDistinguishedName for <%s>\n", name));

		dirStatus = opendirectory_sam_searchattr(&ods_state->session,
				    &recordsArray, kDSStdRecordTypeGroups,
				    kDS1AttrDistinguishedName, name);
	}

	if (dirStatus != eDSNoErr) {
		LOG_DS_ERROR_MSG(DS_TRACE_ERRORS, dirStatus, "odssam_getgrnam",
			("no %s record for '%s'!\n",
			 kDSStdRecordTypeGroups, name));
		return NT_STATUS_UNSUCCESSFUL;
	}

	SMB_ASSERT(recordsArray != NULL);

	/* handle duplicates - currently uses first match in search policy */
        entry = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(recordsArray, 0);
	if (!init_group_from_ods(ods_state, map, entry)) {
		CFRelease(recordsArray);
		return NT_STATUS_UNSUCCESSFUL;
	}

	CFRelease(recordsArray);
	return NT_STATUS_OK;
}


static BOOL init_ods_from_group(struct odssam_privates *ods_state,
	GROUP_MAP *map,
	CFMutableDictionaryRef groupEntry,
	CFMutableDictionaryRef mods)

{
	pstring tmp;

	if (groupEntry == NULL || map == NULL || mods == NULL) {
		DEBUG(module_debug, ("init_ods_from_group: NULL parameters found!\n"));
		return False;
	}

	if (sid_to_string(tmp, &map->sid)) {
		update_user_entry(mods, kDS1AttrSMBSID, tmp);
	}

	if (map->nt_name) {
		update_user_entry(mods, kDS1AttrDistinguishedName,
			map->nt_name);
	}

	if (map->comment) {
		update_user_entry(mods, kDS1AttrComment, map->comment);
	}


	return True;
}

static NTSTATUS odssam_add_group_mapping_entry(struct pdb_methods *methods,
			GROUP_MAP *map)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct odssam_privates *ods_state = get_ods_state(methods);
	tDirStatus dirStatus = eDSNoErr;
	CFMutableArrayRef recordsArray = NULL;
	CFMutableDictionaryRef entry = NULL, groupMods = NULL;
	pstring gid_string;

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	snprintf(gid_string, sizeof(gid_string) - 1, "%i", map->gid);
	DEBUG(module_debug,("odssam_add_group_mapping_entry: gid [%s]\n", gid_string));
	if (opendirectory_sam_searchattr(&ods_state->session,
			&recordsArray, kDSStdRecordTypeGroups,
			kDS1AttrPrimaryGroupID, gid_string) != eDSNoErr) {
			ret = NT_STATUS_UNSUCCESSFUL;
			goto cleanup;
	}

	groupMods = CFDictionaryCreateMutable(NULL, 0,
			    &kCFCopyStringDictionaryKeyCallBacks,
			    &kCFTypeDictionaryValueCallBacks);
	entry = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(recordsArray, 0);
	if (!entry) {
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	if (!init_ods_from_group(ods_state, map, entry, groupMods)) {
		DEBUG(module_debug,("odssam_add_group_mapping_entry: "
			    "init_ods_from_group failed!\n"));

		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}
	ret = NT_STATUS_OK;

	if (CFDictionaryGetCount(groupMods) == 0) {
		DEBUG(module_debug, ("odssam_add_group_mapping_entry: mods is empty\n"));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	dirStatus = add_record_attributes(ods_state, groupMods,
					entry, kDSStdRecordTypeGroups);
	LOG_DS_ERROR(ds_trace, dirStatus, "add_record_attributes");
	if (eDSNoErr != dirStatus) {
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	ret = NT_STATUS_OK;
	DEBUG(module_debug, ("odssam_add_group_mapping_entry: successfully "
		    "modified group %ld in Open Directory\n",
		    (long)map->gid));

cleanup:
	if (groupMods) {
		CFRelease(groupMods);
	}

	return ret;
}

static NTSTATUS odssam_update_group_mapping_entry(struct pdb_methods *methods,
						GROUP_MAP *map)
{
	NTSTATUS ret = NT_STATUS_UNSUCCESSFUL;
	struct odssam_privates *ods_state = get_ods_state(methods);
	tDirStatus dirStatus = eDSNoErr;
	CFMutableArrayRef recordsArray = NULL;
	CFMutableDictionaryRef entry = NULL;
	CFMutableDictionaryRef groupMods = NULL;
	pstring gid_string;

	if (ods_state == NULL) {
		return NT_STATUS_INVALID_HANDLE;
	}

	snprintf(gid_string, sizeof(gid_string) - 1, "%i", map->gid);
	DEBUG(module_debug,("odssam_update_group_mapping_entry: gid [%s]\n", gid_string));
	if (opendirectory_sam_searchattr(&ods_state->session,
			&recordsArray, kDSStdRecordTypeGroups,
			kDS1AttrPrimaryGroupID, gid_string) != eDSNoErr) {
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	groupMods = CFDictionaryCreateMutable(NULL, 0,
				&kCFCopyStringDictionaryKeyCallBacks,
				&kCFTypeDictionaryValueCallBacks);
	entry = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(recordsArray, 0);
	if (!entry) {
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	if (!init_ods_from_group(ods_state, map, entry, groupMods)) {
		DEBUG(module_debug,("odssam_update_group_mapping_entry: "
			    "init_ods_from_group failed!\n"));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	if (CFDictionaryGetCount(groupMods) == 0) {
		DEBUG(module_debug, ("odssam_update_group_mapping_entry: mods is empty\n"));
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	dirStatus = add_record_attributes(ods_state, groupMods,
					entry, kDSStdRecordTypeGroups);
	LOG_DS_ERROR(ds_trace, dirStatus, "add_record_attributes");
	if (eDSNoErr != dirStatus) {
		ret = NT_STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	ret = NT_STATUS_OK;
	DEBUG(module_debug, ("odssam_update_group_mapping_entry: successfully "
		    "modified group %ld in Open Directory\n",
		    (unsigned long)map->gid));

cleanup:
	if (groupMods) {
		CFRelease(groupMods);
	}
	return ret;
}

static NTSTATUS odssam_enum_group_mapping(struct pdb_methods *methods,
					const DOM_SID *sid,
					enum lsa_SidType sid_name_use,
					GROUP_MAP **rmap,
					size_t *num_entries,
					BOOL unix_only)
{
	GROUP_MAP map;
	int entries = 0;

	*num_entries = 0;
	*rmap = NULL;

	if (!NT_STATUS_IS_OK(odssam_setgrpwent(methods, False))) {
		DEBUG(module_debug, ("unable to open passdb\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	while (NT_STATUS_IS_OK(odssam_getgrpwent(methods, &map))) {
		if (sid_name_use != SID_NAME_UNKNOWN &&
		    sid_name_use != map.sid_name_use) {
			DEBUG(module_debug, ("group %s is not of the requested type\n",
				map.nt_name));
			continue;
		}

		if (unix_only==ENUM_ONLY_MAPPED && map.gid==-1) {
			DEBUG(module_debug,("group %s is not mapped\n", map.nt_name));
			continue;
		}

		*rmap = SMB_REALLOC((*rmap), (entries+1)*sizeof(GROUP_MAP));
		if (!(*rmap)) {
			DEBUG(module_debug, ("unable to enlarge group map for %s\n",
				map.nt_name));
			return NT_STATUS_UNSUCCESSFUL;
		}

		(*rmap)[entries] = map;
		entries++;

	}
	odssam_endgrpwent(methods);

	*num_entries = entries;
	return NT_STATUS_OK;
}

static void odssam_free_private_data(void **data)
{
	struct odssam_privates *ods_state = (struct odssam_privates *)(*data);

	DEBUG(module_debug, ("%s: free private data\n", MODULE_NAME));
	opendirectory_disconnect(&ods_state->session);
	talloc_free(ods_state);
	*data = NULL;
}

static NTSTATUS odssam_init_name(struct pdb_methods ** pdb_methods,
		const char *location, const char *name)
{
	struct odssam_privates *ods_state = NULL;
	NTSTATUS status;

	if (!NT_STATUS_IS_OK(status = make_pdb_method(pdb_methods))) {
		return status;
	}

	(*pdb_methods)->name = name;

	(*pdb_methods)->setsampwent = odssam_setsampwent;
	(*pdb_methods)->endsampwent = odssam_endsampwent;
	(*pdb_methods)->getsampwent = odssam_getsampwent;
	(*pdb_methods)->getsampwnam = odssam_getsampwnam;
	(*pdb_methods)->getsampwsid = odssam_getsampwsid;
	(*pdb_methods)->add_sam_account = odssam_add_sam_account;
	(*pdb_methods)->update_sam_account = odssam_update_sam_account;

#ifdef USES_ODGROUPMAPPING
	(*pdb_methods)->getgrsid = odssam_getgrsid;
	(*pdb_methods)->getgrgid = odssam_getgrgid;
	(*pdb_methods)->getgrnam = odssam_getgrnam;
	(*pdb_methods)->add_group_mapping_entry = odssam_add_group_mapping_entry;
	(*pdb_methods)->update_group_mapping_entry = odssam_update_group_mapping_entry;
//	(*pdb_methods)->delete_group_mapping_entry = odssam_delete_group_mapping_entry;
	(*pdb_methods)->enum_group_mapping = odssam_enum_group_mapping;
#endif

	ods_state = TALLOC_ZERO_P(NULL, struct odssam_privates);

	if (!ods_state) {
		return NT_STATUS_NO_MEMORY;
	}

	ods_state->usersArray =
		CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	ods_state->groupsArray =
		CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	if (!ods_state->usersArray || !ods_state->groupsArray) {
		talloc_free(ods_state);
		return NT_STATUS_NO_MEMORY ;
	}

	(*pdb_methods)->private_data = ods_state;
	(*pdb_methods)->free_private_data = odssam_free_private_data;

	odssam_debug_level = debug_add_class(MODULE_NAME);

	if (odssam_open(ods_state) != eDSNoErr) {
		DEBUG(module_debug, ("Unable to access Directory Services\n"));
		talloc_free(ods_state);
		return NT_STATUS_UNSUCCESSFUL;
	}

	return NT_STATUS_OK;
}

static NTSTATUS odssam_init(struct pdb_methods ** pdb_methods,
		const char *location)
{
	return odssam_init_name(pdb_methods, location, MODULE_NAME);
}

static NTSTATUS odssam_init_historic(struct pdb_methods ** pdb_methods,
		const char *location)
{
	return odssam_init_name(pdb_methods, location, "opendirectorysam");
}

NTSTATUS pdb_odsam_init(void)
{
	struct passwd *pwd;

	/* Stash the guest user UID for later. */
	if ((pwd = sys_getpwnam(lp_guestaccount()))) {
		guest_uid = pwd->pw_uid;
	} else {
		DEBUG(0, ("%s: guest account '%s' is not available, using 99\n",
		    MODULE_NAME, lp_guestaccount()));
		guest_uid = 99;
	}

	/* Use "odsam:traceall = yes" to turn on OD query tracing. */
	if (lp_parm_bool(GLOBAL_SECTION_SNUM,
				MODULE_NAME, "traceall", False)) {
		ds_trace = DS_TRACE_ALL;
	}

	module_debug = lp_parm_int(GLOBAL_SECTION_SNUM,
					MODULE_NAME, "msglevel", 100);

	/* Register current module name. */
	smb_register_passdb(PASSDB_INTERFACE_VERSION,
			MODULE_NAME, odssam_init);

	/* Register historic name for 10.4 configs. */
	smb_register_passdb(PASSDB_INTERFACE_VERSION,
			"opendirectorysam", odssam_init_historic);

	return NT_STATUS_OK;
}
