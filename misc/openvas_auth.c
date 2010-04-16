/* OpenVAS Libraries
 * $Id$
 * Description: Authentication mechanism(s).
 *
 * Authors:
 * Matthew Mundell <matt@mundell.ukfsn.org>
 * Michael Wiegand <michael.wiegand@greenbone.net>
 * Felix Wolfsteller <felix.wolfsteller@intevation.de>
 *
 * Copyright:
 * Copyright (C) 2009,2010 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include "openvas_auth.h"

#include "openvas_uuid.h"

#include <errno.h>
#include <gcrypt.h>
#include <glib/gstdio.h>

#ifdef ENABLE_LDAP_AUTH
#include "ldap_auth.h"
#endif /*ENABLE_LDAP_AUTH*/

#define AUTH_CONF_FILE ".auth.conf"

#define GROUP_PREFIX_METHOD "method:"
#define KEY_ORDER "order"


/**
 * @file misc/openvas_auth.c
 *
 * @brief Authentication mechanisms used by openvas-manager and
 * @brief openvas-administrator.
 *
 * Two authentication mechanisms are supported:
 *  - local file authentication. The classical authentication mechanism to
 *    authenticate against files (in PREFIX/var/lib/openvas/users).
 *  - remote ldap authentication. To authenticate against a remote ldap
 *    directory server.
 *
 * Also a mixture of both can be used. To do so, a configuration file
 * (PREFIX/var/lib/openvas/.auth.conf) has to be used and the authentication
 * system has to be initialised with a call to \ref openvas_auth_init and can
 * be freed with \ref openvas_tear_down .
 *
 * The configuration file allows to specify details of a remote ldap
 * authentication and to assign an "order" value to the specified
 * authentication mechanisms. Mechanisms with a lower order will be tried
 * first.
 */

/** @todo explain uuid file placement ("remote-users" vs user dir). Maybe
 *        abondon OPENVAS_USER_DIR, use OPENVAS_STATE_DIR instead. */


/**
 * @brief Numerical representation of the supported authentication methods.
 * @brief Beware to have it in sync with \ref authentication_methods.
 */
enum authentication_method {
  AUTHENTICATION_METHOD_FILE = 0,
  AUTHENTICATION_METHOD_LDAP,
  AUTHENTICATION_METHOD_LAST
};

/** @brief Type for the numerical representation of the supported
 *  @brief authentication methods. */
typedef enum authentication_method auth_method_t;

/**
 * @brief Array of string representations of the supported authentication
 * @brief methods. Beware to have it in sync with \ref authentication_method.
 */
static const gchar* authentication_methods[] = { "file", "ldap", NULL};

/** @brief Flag whether the config file was read. */
static gboolean initialized = FALSE;

/** @brief List of authentication methods. */
static GSList* authenticators = NULL;

/** @brief Representation of an abstract authentication mechanism. */
struct authenticator {
  /** @brief The method of this authenticator. */
  auth_method_t method;
  /** @brief The order. Authenticators with lower order will be tried first. */
  int order;
  /** @brief Authentication callback function. */
  int (*authenticate) (const gchar* user, const gchar* pass, void* data);
  /** @brief Optional data to be passed to the \ref authenticate callback
   *  @brief function. */
  void* data;
};

/** @brief Authenticator type. */
typedef struct authenticator* authenticator_t;


// forward decl.
static int
openvas_authenticate_classic (const gchar * usr, const gchar * pas, void* dat);


/**
 * @brief Return a auth_method_t from string representation (e.g. "ldap").
 *
 * Keep in sync with \ref authentication_methods and
 * \ref authentication_method .
 *
 * @param method The string representation of an auth_method_t (e.g. "file").
 *
 * @return Respective auth_method_t, -1 for unknown.
 */
static auth_method_t
auth_method_from_string (const char* method)
{
  if (method == NULL)
    return -1;

  int i = 0;
  for (i = 0; i < AUTHENTICATION_METHOD_LAST; i++)
    if (!strcmp (method, authentication_methods[i]))
      return i;
  return -1;
}


/**
 * @brief Implements a (GCompareFunc) to add authenticators to the
 * @brief authenticator list, sorted by the order.
 *
 * @param first_auth  First authenticator to be compared.
 * @param second_auth Second authenticator to be compared.
 *
 * @return >0 If the first authenticator should come after the second.
 */
static gint
order_compare (authenticator_t first_auth, authenticator_t second_auth)
{
  return (first_auth->order - second_auth->order);
}


/**
 * @brief Create a fresh authenticator to authenticate against a file.
 *
 * @param order Order of the authenticator.
 *
 * @return A fresh authenticator to authenticate against a file.
 */
static authenticator_t
classic_authenticator_new (int order)
{
  authenticator_t authent = g_malloc0 (sizeof (struct authenticator));
  authent->order = order;
  authent->authenticate = &openvas_authenticate_classic;
  authent->data = (void*) NULL;
  authent->method = AUTHENTICATION_METHOD_FILE;
  return authent;
}

/**
 * @brief Add an authenticator.
 *
 * @param key_file GKeyFile to access for getting data.
 * @param group    Groupname within \ref key_file to query.
 */
static void
add_authenticator (GKeyFile* key_file, const gchar* group)
{
  const char* auth_method_str = group + strlen (GROUP_PREFIX_METHOD);
  auth_method_t auth_method = auth_method_from_string (auth_method_str);
  GError* error = NULL;
  int order = g_key_file_get_integer (key_file, group, KEY_ORDER, &error);
  if (error != NULL)
    {
      g_warning ("No order for authentication method %s specified, "
                 "skipping this entry.\n", group);
      g_error_free (error);
      return;
    }
  switch (auth_method)
    {
      case AUTHENTICATION_METHOD_FILE:
        {
          authenticator_t authent = classic_authenticator_new (order);
          authenticators = g_slist_insert_sorted (authenticators, authent,
                                                  (GCompareFunc) order_compare);
          break;
        }
      case AUTHENTICATION_METHOD_LDAP:
        {
#ifdef ENABLE_LDAP_AUTH
          //int (*authenticate_func) (const gchar* user, const gchar* pass, void* data) = NULL;
          authenticator_t authent = g_malloc0 (sizeof (struct authenticator));
          authent->order = order;
          authent->authenticate = &ldap_authenticate;
          ldap_auth_info_t info = ldap_auth_info_from_key_file (key_file, group);
          authent->data = info;
          authent->method = AUTHENTICATION_METHOD_LDAP;
          authenticators = g_slist_insert_sorted (authenticators, authent,
                                                  (GCompareFunc) order_compare);
#else
          g_warning ("LDAP Authentication was configured, but "
                    "openvas-libraries was not build with "
                    "ldap-support. The configuration entry will "
                    "have no effect.");
#endif /* ENABLE_LDAP_AUTH */
          break;
        }
      default:
        g_warning ("Unsupported authentication method: %s.", group);
        break;
    }
}

/**
 * @brief Initializes the list of authentication methods.
 *
 * Parses PREFIX/var/lib/openvas/.auth.conf and adds respective authenticators
 * to the authenticators list.
 *
 * Call once before calls to openvas_authenticate, otherwise the
 * authentication method will default to file-system based authentication.
 *
 * The list should be freed with \ref openvas_auth_tear_down once no further
 * authentication trials will be done.
 *
 * A warning will be issued if \ref openvas_auth_init is called a second time
 * without a call to \ref openvas_auth_tear_down in between. In this case,
 * no reconfiguration will take place.
 */
void
openvas_auth_init ()
{
  if (initialized == TRUE)
    {
      g_warning ("openvas_auth_init called a second time.");
      return;
    }

  GKeyFile* key_file = g_key_file_new ();
  gchar* config_file = g_build_filename (OPENVAS_USERS_DIR, ".auth.conf",
                                         NULL);
  gboolean loaded = g_key_file_load_from_file (key_file, config_file, G_KEY_FILE_NONE, NULL);
  gchar** groups  = NULL;
  gchar** group   = NULL;
  g_free (config_file);

  if (loaded == FALSE)
    {
      g_key_file_free (key_file);
      initialized = TRUE;
      g_warning ("Authentication configuration could not be loaded.\n");
      return;
    }

  groups = g_key_file_get_groups (key_file, NULL);

  group = groups;
  while (*group != NULL)
    {
      if (g_str_has_prefix (*group, GROUP_PREFIX_METHOD))
        {
          add_authenticator (key_file, *group);
        }
      group++;
    }

  g_key_file_free (key_file);
  g_strfreev (groups);
  initialized = TRUE;
}

/**
 * @brief Free memory associated to authentication configuration.
 *
 * This will have no effect if openvas_auth_init was not called.
 */
void
openvas_auth_tear_down ()
{
  /** @todo Close memleak, destroy list and content. */
}

/**
 * @brief Generate a hexadecimal representation of a message digest.
 *
 * @param gcrypt_algorithm The libgcrypt message digest algorithm used to
 * create the digest (e.g. GCRY_MD_MD5; see the enum gcry_md_algos in
 * gcrypt.h).
 * @param digest The binary representation of the digest.
 *
 * @return A pointer to the hexadecimal representation of the message digest
 * or NULL if an unavailable message digest algorithm was selected.
 */
gchar *
digest_hex (int gcrypt_algorithm, const guchar * digest)
{
  gcry_error_t err = gcry_md_test_algo (gcrypt_algorithm);
  if (err != 0)
    {
      g_warning ("Could not select gcrypt algorithm: %s",
                 gcry_strerror (err));
      return NULL;
    }

  gchar *hex = g_malloc0(gcry_md_get_algo_dlen (gcrypt_algorithm) * 2 + 1);
  int i;

  for (i = 0; i < gcry_md_get_algo_dlen (gcrypt_algorithm); i++)
    {
      g_snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }

  return hex;
}

/**
 * @brief Generate a pair of hashes to be used in the OpenVAS "auth/hash" file
 * for the user.
 *
 * The "auth/hash" file consist of two hashes, h_1 and h_2. h_2 (the "seed")
 * is the message digest of (currently) 256 bytes of random data. h_1 is the
 * message digest of h_2 concatenated with the password in plaintext.
 *
 * The current implementation was taken from the openvas-adduser shell script
 * provided with openvas-server.
 *
 * @param gcrypt_algorithm The libgcrypt message digest algorithm used to
 * create the digest (e.g. GCRY_MD_MD5; see the enum gcry_md_algos in
 * gcrypt.h)
 * @param password The password in plaintext.
 *
 * @return A pointer to a gchar containing the two hashes separated by a
 * space or NULL if an unavailable message digest algorithm was selected.
 */
gchar *
get_password_hashes (int gcrypt_algorithm, const gchar * password)
{
  gcry_error_t err = gcry_md_test_algo (gcrypt_algorithm);
  if (err != 0)
    {
      g_warning ("Could not select gcrypt algorithm: %s",
                 gcry_strerror (err));
      return NULL;
    }

  g_assert (password);

  /* RATS:ignore, is sanely used with gcry_create_nonce and gcry_md_hash_buffer */
  unsigned char *nonce_buffer[256];
  guchar *seed = g_malloc0 (gcry_md_get_algo_dlen (gcrypt_algorithm));
  gchar *seed_hex = NULL;
  gchar *seed_pass = NULL;
  guchar *hash = g_malloc0 (gcry_md_get_algo_dlen (gcrypt_algorithm));
  gchar *hash_hex = NULL;
  gchar *hashes_out = NULL;

  gcry_create_nonce (nonce_buffer, 256);
  gcry_md_hash_buffer (GCRY_MD_MD5, seed, nonce_buffer, 256);
  seed_hex = digest_hex (GCRY_MD_MD5, seed);
  seed_pass = g_strconcat (seed_hex, password, NULL);
  gcry_md_hash_buffer (GCRY_MD_MD5, hash, seed_pass, strlen (seed_pass));
  hash_hex = digest_hex (GCRY_MD_MD5, hash);

  hashes_out = g_strjoin (" ", hash_hex, seed_hex, NULL);

  g_free (seed);
  g_free (seed_hex);
  g_free (seed_pass);
  g_free (hash);
  g_free (hash_hex);

  return hashes_out;
}

/**
 * @brief Authenticate a credential pair against openvas user file contents.
 *
 * @param username Username.
 * @param password Password.
 * @param data     Ignored.
 *
 * @return 0 authentication success, 1 authentication failure, -1 error.
 */
static int
openvas_authenticate_classic (const gchar * username, const gchar * password,
                              void* data)
{
  int gcrypt_algorithm = GCRY_MD_MD5; // FIX whatever configer used
  int ret;
  gchar* actual;
  gchar* expect;
  GError* error = NULL;
  gchar *seed_pass = NULL;
  guchar *hash = g_malloc0 (gcry_md_get_algo_dlen (gcrypt_algorithm));
  gchar *hash_hex = NULL;
  gchar **seed_hex;
  gchar **split;

  gchar *file_name = g_build_filename (OPENVAS_USERS_DIR,
                                       username,
                                       "auth",
                                       "hash",
                                       NULL);
  g_file_get_contents (file_name, &actual, NULL, &error);
  g_free (file_name);
  if (error)
    {
      g_free (hash);
      g_error_free (error);
      return 1;
    }

  split = g_strsplit_set (g_strchomp (actual), " ", 2);
  seed_hex = split + 1;
  if (*split == NULL || *seed_hex == NULL)
    {
      g_warning ("Failed to split auth contents.");
      g_free (hash);
      g_strfreev (split);
      /** @todo evaluate poss. memleak: actual */
      return -1;
    }

  seed_pass = g_strconcat (*seed_hex, password, NULL);
  gcry_md_hash_buffer (GCRY_MD_MD5, hash, seed_pass, strlen (seed_pass));
  hash_hex = digest_hex (GCRY_MD_MD5, hash);

  expect = g_strjoin (" ", hash_hex, *seed_hex, NULL);

  g_strfreev (split);
  g_free (seed_pass);
  g_free (hash);
  g_free (hash_hex);

  ret = strcmp (expect, actual) ? 1 : 0;
  g_free (expect);
  g_free (actual);
  return ret;
}

/**
 * @brief Authenticate a credential pair.
 *
 * Uses the configurable authenticators list, if available.
 * Defaults to file-based (openvas users directory) authentication otherwise.
 *
 * @param username Username.
 * @param password Password.
 *
 * @return 0 authentication success, otherwise the result of the last
 *         authentication trial: 1 authentication failure, -1 error.
 */
int
openvas_authenticate (const gchar * username, const gchar * password)
{
  if (initialized == FALSE || authenticators == NULL)
    return openvas_authenticate_classic (username, password, NULL);

  // Try each authenticator in the list.
  int ret = -1;
  GSList* item = authenticators;
  while (item)
    {
      authenticator_t authent = (authenticator_t) item->data;
      ret = authent->authenticate (username, password, authent->data);
      g_debug ("Authentication trial, order %d, method %s -> %d.", authent->order,
               authentication_methods[authent->method], ret);

      // Return if successfull
      if (ret == 0)
        return 0;

      item = g_slist_next (item);
    }
  return ret;
}


/**
 * @brief Authenticate a credential pair and expose the method used.
 *
 * Uses the configurable authenticators list, if available.
 * Defaults to file-based (openvas users directory) authentication otherwise.
 *
 * @param username Username.
 * @param password Password.
 * @param method[out] Return location for the method that was used to
 *                    authenticate the credential pair.
 *
 * @return 0 authentication success, otherwise the result of the last
 *         authentication trial: 1 authentication failure, -1 error.
 */
static int
openvas_authenticate_method (const gchar * username, const gchar * password,
                             auth_method_t* method)
{
  *method = AUTHENTICATION_METHOD_FILE;
  if (initialized == FALSE || authenticators == NULL)
    return openvas_authenticate_classic (username, password, NULL);

  // Try each authenticator in the list.
  int ret = -1;
  GSList* item = authenticators;
  while (item)
    {
      authenticator_t authent = (authenticator_t) item->data;
      ret = authent->authenticate (username, password, authent->data);
      g_debug ("Authentication trial, order %d, method %s -> %d. (w/method)", authent->order,
               authentication_methods[authent->method], ret);

      // Return if successfull
      if (ret == 0)
        {
          *method = authent->method;
          return 0;
        }

      item = g_slist_next (item);
    }
  return ret;
}


/**
 * @brief Return the UUID of a user from the OpenVAS user UUID file.
 *
 * If the user exists, ensure that the user has a UUID (create that file).
 *
 * @param[in]  name   User name.
 *
 * @return UUID of given user if user exists, else NULL.
 */
static gchar *
openvas_user_uuid_method (const char *name, const auth_method_t method)
{
  gchar *user_dir = (method == AUTHENTICATION_METHOD_FILE)
                    ? g_build_filename (OPENVAS_USERS_DIR, name, NULL)
                    : g_build_filename (OPENVAS_STATE_DIR,
                                        "users-remote",
                                        authentication_methods[method],
                                        name, NULL);

  if (!g_file_test (user_dir, G_FILE_TEST_EXISTS))
    {
      // Assume that the user does exist, and is remotely authenticated.
      // Create a user dir to store the uuid.
      /** @todo Handle error case. */
      g_mkdir_with_parents (user_dir, 0700);
    }

    {
      gchar *uuid_file = g_build_filename (user_dir, "uuid", NULL);
      // File exists, get its content (the uuid).
      if (g_file_test (uuid_file, G_FILE_TEST_EXISTS))
        {
          gsize size;
          gchar *uuid;
          if (g_file_get_contents (uuid_file, &uuid, &size, NULL))
            {
              if (strlen (uuid) < 36)
                g_free (uuid);
              else
                {
                  g_free (user_dir);
                  g_free (uuid_file);
                  /* Drop any trailing characters. */
                  uuid[36] = '\0';
                  return uuid;
                }
            }
        }
      // File does not exists, create file, set (new) uuid as content.
      else
        {
          gchar *contents;
          char *uuid;

          uuid = openvas_uuid_make ();
          if (uuid == NULL)
            {
              g_free (user_dir);
              g_free (uuid_file);
              return NULL;
            }

          contents = g_strdup_printf ("%s\n", uuid);

          if (g_file_set_contents (uuid_file, contents, -1, NULL))
            {
              g_free (contents);
              g_free (user_dir);
              g_free (uuid_file);
              return uuid;
            }

          g_free (contents);
          free (uuid);
        }
      g_free (uuid_file);
    }
  g_free (user_dir);
  return NULL;
}


/**
 * @brief Authenticate a credential pair, returning the user UUID.
 *
 * @param  username  Username.
 * @param  password  Password.
 * @param  uuid      UUID return.
 *
 * @return 0 authentication success, 1 authentication failure, -1 error.
 */
int
openvas_authenticate_uuid (const gchar * username, const gchar * password,
                           gchar **uuid)
{
  int ret;

  // Authenticate
  auth_method_t method;
  ret = openvas_authenticate_method (username, password, &method);
  if (ret)
    {
      return ret;
    }

  // Get the uuid from file (create it if it did not yet exist).
  *uuid = openvas_user_uuid_method (username, method);
  if (*uuid)
    return 0;
  return -1;
}


/**
 * @brief Return the UUID of a user from the OpenVAS user UUID file.
 *
 * If the user exists, ensure that the user has a UUID (create that file).
 *
 * @deprecated  Use \ref openvas_authenticate_uuid to receive users uuid where
 *              you can. This leaves an issue in manager/schedular, that is
 *              solveable by storing a uuid instead of manage_auth_allow_all
 *              in openvasmd.
 *
 * @param[in]  name   User name.
 *
 * @return UUID of given user if user exists, else NULL.
 */
gchar *
openvas_user_uuid (const char *name)
{
  gchar *user_dir = g_build_filename (OPENVAS_USERS_DIR, name, NULL);
  if (!g_file_test (user_dir, G_FILE_TEST_EXISTS))
    {
      // Assume that the user does exist, but is remotely authenticated.
      // Create a user dir to store the uuid.
      /** @todo Resolve the issue that for remote authentication a directory
       *        and uuid file has to be created. Also, handle error case. */
      g_mkdir_with_parents (user_dir, 0700);
    }

    {
      gchar *uuid_file = g_build_filename (user_dir, "uuid", NULL);
      if (g_file_test (uuid_file, G_FILE_TEST_EXISTS))
        {
          gsize size;
          gchar *uuid;
          if (g_file_get_contents (uuid_file, &uuid, &size, NULL))
            {
              if (strlen (uuid) < 36)
                g_free (uuid);
              else
                {
                  g_free (user_dir);
                  g_free (uuid_file);
                  /* Drop any trailing characters. */
                  uuid[36] = '\0';
                  return uuid;
                }
            }
        }
      else
        {
          gchar *contents;
          char *uuid;

          uuid = openvas_uuid_make ();
          if (uuid == NULL)
            {
              g_free (user_dir);
              g_free (uuid_file);
              return NULL;
            }

          contents = g_strdup_printf ("%s\n", uuid);

          if (g_file_set_contents (uuid_file, contents, -1, NULL))
            {
              g_free (contents);
              g_free (user_dir);
              g_free (uuid_file);
              return uuid;
            }

          g_free (contents);
          free (uuid);
        }
      g_free (uuid_file);
    }
  g_free (user_dir);
  return NULL;
}


/**
 * @brief Check if a user has administrative privileges.
 *
 * The check for administrative privileges is currently done by looking for an
 * "isadmin" file in the user directory.
 *
 * @param username Username.
 *
 * @return 1 user has administrative privileges, 0 user does not have
 * administrative privileges
 */
int
openvas_is_user_admin (const gchar * username)
{
  gchar *file_name = g_build_filename (OPENVAS_USERS_DIR,
                                       username,
                                       "isadmin",
                                       NULL);
  gboolean file_exists = g_file_test (file_name, G_FILE_TEST_EXISTS);

  g_free (file_name);
  return file_exists;
}

/**
 * @brief Set the role of a user.
 *
 * @param username Username.
 * @param role Role.
 *
 * @return 0 success, -1 failure.
 */
int
openvas_set_user_role (const gchar * username, const gchar * role)
{
  int ret = -1;
  gchar *file_name;

  file_name = g_build_filename (OPENVAS_USERS_DIR,
                                username,
                                "isadmin",
                                NULL);

  if (strcmp (role, "User") == 0)
    {
      if (g_remove (file_name))
        {
          if (errno == ENOENT) ret = 0;
        }
      else
        ret = 0;
    }
  else if (strcmp (role, "Admin") == 0
           && g_file_set_contents (file_name, "", 0, NULL))
    {
      g_chmod (file_name, 0600);
      ret = 0;
    }

  g_free (file_name);
  return ret;
}
