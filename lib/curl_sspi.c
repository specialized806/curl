/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#include "curl_setup.h"

#ifdef USE_WINDOWS_SSPI

#include <curl/curl.h>
#include "curl_sspi.h"
#include "strdup.h"
#include "curlx/multibyte.h"
#include "system_win32.h"
#include "curlx/warnless.h"

/* The last #include files should be: */
#include "curl_memory.h"
#include "memdebug.h"

/* Pointer to SSPI dispatch table */
PSecurityFunctionTable Curl_pSecFn = NULL;

/*
 * Curl_sspi_global_init()
 *
 * This is used to load the Security Service Provider Interface (SSPI)
 * dynamic link library portably across all Windows versions, without
 * the need to directly link libcurl, nor the application using it, at
 * build time.
 *
 * Once this function has been executed, Windows SSPI functions can be
 * called through the Security Service Provider Interface dispatch table.
 *
 * Parameters:
 *
 * None.
 *
 * Returns CURLE_OK on success.
 */
CURLcode Curl_sspi_global_init(void)
{
  /* If security interface is not yet initialized try to do this */
  if(!Curl_pSecFn) {
    /* Get pointer to Security Service Provider Interface dispatch table */
#ifdef __MINGW32CE__
    Curl_pSecFn = InitSecurityInterfaceW();
#else
    Curl_pSecFn = InitSecurityInterface();
#endif
    if(!Curl_pSecFn)
      return CURLE_FAILED_INIT;
  }

  return CURLE_OK;
}

/*
 * Curl_sspi_global_cleanup()
 *
 * This deinitializes the Security Service Provider Interface from libcurl.
 *
 * Parameters:
 *
 * None.
 */
void Curl_sspi_global_cleanup(void)
{
  if(Curl_pSecFn) {
    Curl_pSecFn = NULL;
  }
}

/*
 * Curl_create_sspi_identity()
 *
 * This is used to populate an SSPI identity structure based on the supplied
 * username and password.
 *
 * Parameters:
 *
 * userp    [in]     - The username in the format User or Domain\User.
 * passwdp  [in]     - The user's password.
 * identity [in/out] - The identity structure.
 *
 * Returns CURLE_OK on success.
 */
CURLcode Curl_create_sspi_identity(const char *userp, const char *passwdp,
                                   SEC_WINNT_AUTH_IDENTITY *identity)
{
  xcharp_u useranddomain;
  xcharp_u user, dup_user;
  xcharp_u domain, dup_domain;
  xcharp_u passwd, dup_passwd;
  size_t domlen = 0;

  domain.const_tchar_ptr = TEXT("");

  /* Initialize the identity */
  memset(identity, 0, sizeof(*identity));

  useranddomain.tchar_ptr = curlx_convert_UTF8_to_tchar(userp);
  if(!useranddomain.tchar_ptr)
    return CURLE_OUT_OF_MEMORY;

  user.const_tchar_ptr = _tcschr(useranddomain.const_tchar_ptr, TEXT('\\'));
  if(!user.const_tchar_ptr)
    user.const_tchar_ptr = _tcschr(useranddomain.const_tchar_ptr, TEXT('/'));

  if(user.tchar_ptr) {
    domain.tchar_ptr = useranddomain.tchar_ptr;
    domlen = user.tchar_ptr - useranddomain.tchar_ptr;
    user.tchar_ptr++;
  }
  else {
    user.tchar_ptr = useranddomain.tchar_ptr;
    domain.const_tchar_ptr = TEXT("");
    domlen = 0;
  }

  /* Setup the identity's user and length */
  dup_user.tchar_ptr = _tcsdup(user.tchar_ptr);
  if(!dup_user.tchar_ptr) {
    curlx_unicodefree(useranddomain.tchar_ptr);
    return CURLE_OUT_OF_MEMORY;
  }
  identity->User = dup_user.tbyte_ptr;
  identity->UserLength = curlx_uztoul(_tcslen(dup_user.tchar_ptr));
  dup_user.tchar_ptr = NULL;

  /* Setup the identity's domain and length */
  dup_domain.tchar_ptr = malloc(sizeof(TCHAR) * (domlen + 1));
  if(!dup_domain.tchar_ptr) {
    curlx_unicodefree(useranddomain.tchar_ptr);
    return CURLE_OUT_OF_MEMORY;
  }
  _tcsncpy(dup_domain.tchar_ptr, domain.tchar_ptr, domlen);
  *(dup_domain.tchar_ptr + domlen) = TEXT('\0');
  identity->Domain = dup_domain.tbyte_ptr;
  identity->DomainLength = curlx_uztoul(domlen);
  dup_domain.tchar_ptr = NULL;

  curlx_unicodefree(useranddomain.tchar_ptr);

  /* Setup the identity's password and length */
  passwd.tchar_ptr = curlx_convert_UTF8_to_tchar(passwdp);
  if(!passwd.tchar_ptr)
    return CURLE_OUT_OF_MEMORY;
  dup_passwd.tchar_ptr = _tcsdup(passwd.tchar_ptr);
  if(!dup_passwd.tchar_ptr) {
    curlx_unicodefree(passwd.tchar_ptr);
    return CURLE_OUT_OF_MEMORY;
  }
  identity->Password = dup_passwd.tbyte_ptr;
  identity->PasswordLength = curlx_uztoul(_tcslen(dup_passwd.tchar_ptr));
  dup_passwd.tchar_ptr = NULL;

  curlx_unicodefree(passwd.tchar_ptr);

  /* Setup the identity's flags */
  identity->Flags = (unsigned long)
#ifdef UNICODE
    SEC_WINNT_AUTH_IDENTITY_UNICODE;
#else
    SEC_WINNT_AUTH_IDENTITY_ANSI;
#endif

  return CURLE_OK;
}

/*
 * Curl_sspi_free_identity()
 *
 * This is used to free the contents of an SSPI identifier structure.
 *
 * Parameters:
 *
 * identity [in/out] - The identity structure.
 */
void Curl_sspi_free_identity(SEC_WINNT_AUTH_IDENTITY *identity)
{
  if(identity) {
    Curl_safefree(identity->User);
    Curl_safefree(identity->Password);
    Curl_safefree(identity->Domain);
  }
}

#endif /* USE_WINDOWS_SSPI */
