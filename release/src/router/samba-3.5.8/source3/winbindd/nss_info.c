/*
   Unix SMB/CIFS implementation.
   Idmap NSS headers

   Copyright (C) Gerald Carter             2006
   Copyright (C) Michael Adam 2008

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "nss_info.h"

static struct nss_function_entry *backends = NULL;
static struct nss_function_entry *default_backend = NULL;
static struct nss_domain_entry *nss_domain_list = NULL;

/**********************************************************************
 Get idmap nss methods.
**********************************************************************/

static struct nss_function_entry *nss_get_backend(const char *name )
{
	struct nss_function_entry *entry = backends;

	for(entry = backends; entry; entry = entry->next) {
		if ( strequal(entry->name, name) )
			return entry;
	}

	return NULL;
}

/*********************************************************************
 Allow a module to register itself as a backend.
**********************************************************************/

 NTSTATUS smb_register_idmap_nss(int version, const char *name, struct nss_info_methods *methods)
{
	struct nss_function_entry *entry;

	if ((version != SMB_NSS_INFO_INTERFACE_VERSION)) {
		DEBUG(0, ("smb_register_idmap_nss: Failed to register idmap_nss module.\n"
			  "The module was compiled against SMB_NSS_INFO_INTERFACE_VERSION %d,\n"
			  "current SMB_NSS_INFO_INTERFACE_VERSION is %d.\n"
			  "Please recompile against the current version of samba!\n",
			  version, SMB_NSS_INFO_INTERFACE_VERSION));
		return NT_STATUS_OBJECT_TYPE_MISMATCH;
	}

	if (!name || !name[0] || !methods) {
		DEBUG(0,("smb_register_idmap_nss: called with NULL pointer or empty name!\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if ( nss_get_backend(name) ) {
		DEBUG(0,("smb_register_idmap_nss: idmap module %s "
			 "already registered!\n", name));
		return NT_STATUS_OBJECT_NAME_COLLISION;
	}

	entry = SMB_XMALLOC_P(struct nss_function_entry);
	entry->name = smb_xstrdup(name);
	entry->methods = methods;

	DLIST_ADD(backends, entry);
	DEBUG(5, ("smb_register_idmap_nss: Successfully added idmap "
		  "nss backend '%s'\n", name));

	return NT_STATUS_OK;
}

/********************************************************************
 *******************************************************************/

static bool parse_nss_parm( const char *config, char **backend, char **domain )
{
	char *p;
	char *q;
	int len;

	*backend = *domain = NULL;

	if ( !config )
		return False;

	p = strchr( config, ':' );

	/* if no : then the string must be the backend name only */

	if ( !p ) {
		*backend = SMB_STRDUP( config );
		return (*backend != NULL);
	}

	/* split the string and return the two parts */

	if ( strlen(p+1) > 0 ) {
		*domain = SMB_STRDUP( p+1 );
	}

	len = PTR_DIFF(p,config)+1;
	if ( (q = SMB_MALLOC_ARRAY( char, len )) == NULL ) {
		SAFE_FREE( *backend );
		return False;
	}

	StrnCpy( q, config, len-1);
	q[len-1] = '\0';
	*backend = q;

	return True;
}

static NTSTATUS nss_domain_list_add_domain(const char *domain,
					   struct nss_function_entry *nss_backend)
{
	struct nss_domain_entry *nss_domain;

	nss_domain = TALLOC_ZERO_P(nss_domain_list, struct nss_domain_entry);
	if (!nss_domain) {
		DEBUG(0, ("nss_domain_list_add_domain: talloc() failure!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	nss_domain->backend = nss_backend;
	if (domain) {
		nss_domain->domain  = talloc_strdup(nss_domain, domain);
		if (!nss_domain->domain) {
			DEBUG(0, ("nss_domain_list_add_domain: talloc() "
				  "failure!\n"));
			TALLOC_FREE(nss_domain);
			return NT_STATUS_NO_MEMORY;
		}
	}

	nss_domain->init_status = nss_domain->backend->methods->init(nss_domain);
	if (!NT_STATUS_IS_OK(nss_domain->init_status))  {
#if defined(RTCONFIG_HND_ROUTER_BE_4916)
		DEBUG(0, ("nss_init_samba: Failed to init backend '%s' for domain "
			  "'%s'!\n", nss_backend->name, nss_domain->domain));
#else
		DEBUG(0, ("nss_init: Failed to init backend '%s' for domain "
			  "'%s'!\n", nss_backend->name, nss_domain->domain));
#endif
	}

	DLIST_ADD(nss_domain_list, nss_domain);

	DEBUG(10, ("Added domain '%s' with backend '%s' to nss_domain_list.\n",
		   domain, nss_backend->name));

	return NT_STATUS_OK;
}

/********************************************************************
 Each nss backend must not store global state, but rather be able
 to initialize the state on a per domain basis.
 *******************************************************************/

#if defined(RTCONFIG_HND_ROUTER_BE_4916)
 NTSTATUS nss_init_samba( const char **nss_list )
#else
 NTSTATUS nss_init( const char **nss_list )
#endif
{
	NTSTATUS status;
	static NTSTATUS nss_initialized = NT_STATUS_UNSUCCESSFUL;
	int i;
	char *backend, *domain;
	struct nss_function_entry *nss_backend;

	/* check for previous successful initializations */

	if ( NT_STATUS_IS_OK(nss_initialized) )
		return NT_STATUS_OK;

	/* The "template" backend should alqays be registered as it
	   is a static module */

	if ( (nss_backend = nss_get_backend( "template" )) == NULL ) {
		static_init_nss_info;
	}

	/* Create the list of nss_domains (loading any shared plugins
	   as necessary) */

	for ( i=0; nss_list && nss_list[i]; i++ ) {

		if ( !parse_nss_parm(nss_list[i], &backend, &domain) ) {
#if defined(RTCONFIG_HND_ROUTER_BE_4916)
			DEBUG(0,("nss_init_samba: failed to parse \"%s\"!\n",
				 nss_list[i]));
#else
			DEBUG(0,("nss_init: failed to parse \"%s\"!\n",
				 nss_list[i]));
#endif
			continue;
		}

		DEBUG(10, ("parsed backend = '%s', domain = '%s'\n",
			   backend, domain));

		/* validate the backend */

		if ( (nss_backend = nss_get_backend( backend )) == NULL ) {
			/* attempt to register the backend */
			status = smb_probe_module( "nss_info", backend );
			if ( !NT_STATUS_IS_OK(status) ) {
				continue;
			}

			/* try again */
			if ( (nss_backend = nss_get_backend( backend )) == NULL ) {
#if defined(RTCONFIG_HND_ROUTER_BE_4916)
				DEBUG(0,("nss_init_samba: unregistered backend %s!.  Skipping\n",
					 backend));
#else
				DEBUG(0,("nss_init: unregistered backend %s!.  Skipping\n",
					 backend));
#endif
				continue;
			}
		}

		/*
		 * The first config item of the list without an explicit domain
		 * is treated as the default nss info backend.
		 */
		if ((domain == NULL) && (default_backend == NULL)) {
#if defined(RTCONFIG_HND_ROUTER_BE_4916)
			DEBUG(10, ("nss_init_samba: using '%s' as default backend.\n",
				   backend));
#else
			DEBUG(10, ("nss_init: using '%s' as default backend.\n",
				   backend));
#endif
			default_backend = nss_backend;
		}

		status = nss_domain_list_add_domain(domain, nss_backend);
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}

		/* cleanup */

		SAFE_FREE( backend );
		SAFE_FREE( domain );
	}

	if ( !nss_domain_list ) {
#if defined(RTCONFIG_HND_ROUTER_BE_4916)
		DEBUG(3,("nss_init_samba: no nss backends configured.  "
			 "Defaulting to \"template\".\n"));
#else
		DEBUG(3,("nss_init: no nss backends configured.  "
			 "Defaulting to \"template\".\n"));
#endif


		/* we shouild default to use template here */
	}

	nss_initialized = NT_STATUS_OK;

	return NT_STATUS_OK;
}

/********************************************************************
 *******************************************************************/

static struct nss_domain_entry *find_nss_domain( const char *domain )
{
	NTSTATUS status;
	struct nss_domain_entry *p;

#if defined(RTCONFIG_HND_ROUTER_BE_4916)
	status = nss_init_samba( lp_winbind_nss_info() );
#else
	status = nss_init( lp_winbind_nss_info() );
#endif
	if ( !NT_STATUS_IS_OK(status) ) {
		DEBUG(4,("nss_get_info: Failed to init nss_info API (%s)!\n",
			 nt_errstr(status)));
		return NULL;
	}

	for ( p=nss_domain_list; p; p=p->next ) {
		if ( strequal( p->domain, domain ) )
			break;
	}

	/* If we didn't find a match, then use the default nss backend */

	if ( !p ) {
		if (!default_backend) {
			return NULL;
		}

		status = nss_domain_list_add_domain(domain, default_backend);
		if (!NT_STATUS_IS_OK(status)) {
			return NULL;
		}

		/*
		 * HACK ALERT:
		 * Here, we use the fact that the new domain was added at
		 * the beginning of the list...
		 */
		p = nss_domain_list;
	}

	if ( !NT_STATUS_IS_OK( p->init_status ) ) {
	       p->init_status = p->backend->methods->init( p );
	}

	return p;
}

/********************************************************************
 *******************************************************************/

NTSTATUS nss_get_info( const char *domain, const DOM_SID *user_sid,
		       TALLOC_CTX *ctx,
		       ADS_STRUCT *ads, LDAPMessage *msg,
		       const char **homedir, const char **shell,
		       const char **gecos, gid_t *p_gid)
{
	struct nss_domain_entry *p;
	struct nss_info_methods *m;

	DEBUG(10, ("nss_get_info called for sid [%s] in domain '%s'\n",
		   sid_string_dbg(user_sid), domain?domain:"NULL"));

	if ( (p = find_nss_domain( domain )) == NULL ) {
		DEBUG(4,("nss_get_info: Failed to find nss domain pointer for %s\n",
			 domain ));
		return NT_STATUS_NOT_FOUND;
	}

	m = p->backend->methods;

	return m->get_nss_info( p, user_sid, ctx, ads, msg,
				homedir, shell, gecos, p_gid );
}

/********************************************************************
 *******************************************************************/

 NTSTATUS nss_map_to_alias( TALLOC_CTX *mem_ctx, const char *domain,
			    const char *name, char **alias )
{
	struct nss_domain_entry *p;
	struct nss_info_methods *m;

	if ( (p = find_nss_domain( domain )) == NULL ) {
		DEBUG(4,("nss_map_to_alias: Failed to find nss domain pointer for %s\n",
			 domain ));
		return NT_STATUS_NOT_FOUND;
	}

	m = p->backend->methods;

	return m->map_to_alias(mem_ctx, p, name, alias);
}


/********************************************************************
 *******************************************************************/

 NTSTATUS nss_map_from_alias( TALLOC_CTX *mem_ctx, const char *domain,
			      const char *alias, char **name )
{
	struct nss_domain_entry *p;
	struct nss_info_methods *m;

	if ( (p = find_nss_domain( domain )) == NULL ) {
		DEBUG(4,("nss_map_from_alias: Failed to find nss domain pointer for %s\n",
			 domain ));
		return NT_STATUS_NOT_FOUND;
	}

	m = p->backend->methods;

	return m->map_from_alias( mem_ctx, p, alias, name );
}

/********************************************************************
 *******************************************************************/

 NTSTATUS nss_close( const char *parameters )
{
	struct nss_domain_entry *p = nss_domain_list;
	struct nss_domain_entry *q;

	while ( p && p->backend && p->backend->methods ) {
		/* close the backend */
		p->backend->methods->close_fn();

		/* free the memory */
		q = p;
		p = p->next;
		TALLOC_FREE( q );
	}

	return NT_STATUS_OK;
}

