/*
   myldap.c - simple interface to do LDAP requests
   Parts of this file were part of the nss_ldap library (as ldap-nss.c)
   which has been forked into the nss-ldapd library.

   Copyright (C) 1997-2006 Luke Howard
   Copyright (C) 2006, 2007 West Consulting
   Copyright (C) 2006, 2007 Arthur de Jong

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA
*/

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <errno.h>
#include <netinet/in.h>
#include <ldap.h>
#ifdef HAVE_LDAP_SSL_H
#include <ldap_ssl.h>
#endif
#ifdef HAVE_GSSLDAP_H
#include <gssldap.h>
#endif
#ifdef HAVE_GSSSASL_H
#include <gsssasl.h>
#endif
/* Try to handle systems with both SASL libraries installed */
#if defined(HAVE_SASL_SASL_H) && defined(HAVE_SASL_AUXPROP_REQUEST)
#include <sasl/sasl.h>
#elif defined(HAVE_SASL_H)
#include <sasl.h>
#endif
#include <ctype.h>

/* FIXME: get rid of this */
#include <nss.h>

#include "myldap.h"
#include "pagectrl.h"
#include "common.h"
#include "log.h"
#include "cfg.h"
#include "attmap.h"
#include "compat/ldap.h"
#include "common/dict.h"

/* the maximum number of searches per session */
#define MAX_SEARCHES_IN_SESSION 4

/*
 * convenient wrapper around pointer into global config list, and a
 * connection to an LDAP server.
 */
struct ldap_session
{
  /* the connection */
  LDAP *ls_conn;
  /* timestamp of last activity */
  time_t ls_timestamp;
  /* index into ldc_uris: currently connected LDAP uri */
  int ls_current_uri;
  /* a list of searches registered with this session */
  struct myldap_search *searches[MAX_SEARCHES_IN_SESSION];
};

/* A search description set as returned by myldap_search(). */
struct myldap_search
{
  /* reference to the session */
  MYLDAP_SESSION *session;
  /* the parameters descibing the search */
  const char *base;
  int scope;
  const char *filter;
  char **attrs;
  /* a pointer to the current result entry, used for
     freeing resource allocated with that entry */
  MYLDAP_ENTRY *entry;
  /* LDAP message id */
  int msgid;
  /* the last result that was returned by ldap_result() */
  LDAPMessage *msg;
  /* cookie for paged searches */
  struct berval *cookie;
};

/* A single entry from the LDAP database as returned by
   myldap_get_entry(). */
struct myldap_entry
{
  /* reference to the search to be used to get parameters
     (e.g. LDAP connection) for other calls */
  MYLDAP_SEARCH *search;
  /* reference to the LDAP message describing the result */
  LDAPMessage *msg;
  /* the DN */
  const char *dn;
  /* a cached version of the exploded rdn */
  char **exploded_rdn;
  /* a cache of attribute to value list */
  DICT *attributevalues;
};

static MYLDAP_ENTRY *myldap_entry_new(MYLDAP_SEARCH *search,LDAPMessage *msg)
{
  MYLDAP_ENTRY *entry;
  /* Note: as an alternative we could embed the myldap_entry into the
     myldap_search struct to save on malloc() and free() calls. */
  /* allocate new entry */
  entry=(MYLDAP_ENTRY *)malloc(sizeof(struct myldap_entry));
  if (entry==NULL)
  {
    log_log(LOG_CRIT,"myldap_entry_new(): malloc() failed to allocate memory");
    exit(EXIT_FAILURE);
  }
  /* fill in fields */
  entry->search=search;
  entry->msg=msg;
  entry->dn=NULL;
  entry->exploded_rdn=NULL;
  entry->attributevalues=dict_new();
  /* return the fresh entry */
  return entry;
}

static void myldap_entry_free(MYLDAP_ENTRY *entry)
{
  char **values;
  /* free the DN */
  if (entry->dn!=NULL)
    ldap_memfree((char *)entry->dn);
  /* free the exploded RDN */
  if (entry->exploded_rdn!=NULL)
    ldap_value_free(entry->exploded_rdn);
  /* free all attribute values */
  dict_values_first(entry->attributevalues);
  while ((values=(char **)dict_values_next(entry->attributevalues))!=NULL)
    ldap_value_free(values);
  dict_free(entry->attributevalues);
  /* we don't need the result anymore, ditch it. */
  ldap_msgfree(entry->search->msg);
  entry->search->msg=NULL;
  /* apparently entry->msg does not need to be freed */
  entry->msg=NULL;
  /* free the actual memory for the struct */
  free(entry);
}

static MYLDAP_SEARCH *myldap_search_new(
        MYLDAP_SESSION *session,
        const char *base,int scope,const char *filter,const char **attrs)
{
  char *buffer;
  MYLDAP_SEARCH *search;
  int i;
  size_t sz;
  /* figure out size for new memory block to allocate
     this has the advantage that we can free the whole lot with one call */
  sz=sizeof(struct myldap_search);
  sz+=strlen(base)+1+strlen(filter)+1;
  for (i=0;attrs[i]!=NULL;i++)
    sz+=strlen(attrs[i])+1;
  sz+=(i+1)*sizeof(char *);
  /* allocate new results memory region */
  buffer=(char *)malloc(sz);
  if (buffer==NULL)
  {
    log_log(LOG_CRIT,"myldap_search_new(): malloc() failed to allocate memory");
    exit(EXIT_FAILURE);
  }
  /* initialize struct */
  search=(MYLDAP_SEARCH *)(buffer);
  buffer+=sizeof(struct myldap_search);
  /* save pointer to session */
  search->session=session;
  /* initialize array of attributes */
  search->attrs=(char **)buffer;
  buffer+=(i+1)*sizeof(char *);
  /* copy base */
  strcpy(buffer,base);
  search->base=buffer;
  buffer+=strlen(base)+1;
  /* just plainly store scope */
  search->scope=scope;
  /* copy filter */
  strcpy(buffer,filter);
  search->filter=buffer;
  buffer+=strlen(filter)+1;
  /* copy attributes themselves */
  for (i=0;attrs[i]!=NULL;i++)
  {
    strcpy(buffer,attrs[i]);
    search->attrs[i]=buffer;
    buffer+=strlen(attrs[i])+1;
  }
  search->attrs[i]=NULL;
  /* initialize context */
  search->cookie=NULL;
  search->msg=NULL;
  search->msgid=-1;
  /* clear result entry */
  search->entry=NULL;
  /* return the new search struct */
  return search;
}

static void myldap_search_free(MYLDAP_SEARCH *search)
{
  /* free any search entries */
  if (search->entry!=NULL)
    myldap_entry_free(search->entry);
  /* free read messages */
  if (search->msg!=NULL)
    ldap_msgfree(search->msg);
  /* clean up cookie */
  if (search->cookie!=NULL)
    ber_bvfree(search->cookie);
  /* free the storage we allocated */
  free(search);
}

static MYLDAP_SESSION *myldap_session_new(void)
{
  MYLDAP_SESSION *session;
  int i;
  /* allocate memory for the session storage */
  session=(struct ldap_session *)malloc(sizeof(struct ldap_session));
  if (session==NULL)
  {
    log_log(LOG_CRIT,"malloc() failed to allocate memory");
    exit(EXIT_FAILURE);
  }
  /* initialize the session */
  session->ls_conn=NULL;
  session->ls_timestamp=0;
  session->ls_current_uri=0;
  for (i=0;i<MAX_SEARCHES_IN_SESSION;i++)
    session->searches[i]=NULL;
  /* return the new session */
  return session;
}

static enum nss_status do_map_error(int rc)
{
  switch (rc)
  {
    case LDAP_SUCCESS:
    case LDAP_SIZELIMIT_EXCEEDED:
    case LDAP_TIMELIMIT_EXCEEDED:
      return NSS_STATUS_SUCCESS;
      break;
    case LDAP_NO_SUCH_ATTRIBUTE:
    case LDAP_UNDEFINED_TYPE:
    case LDAP_INAPPROPRIATE_MATCHING:
    case LDAP_CONSTRAINT_VIOLATION:
    case LDAP_TYPE_OR_VALUE_EXISTS:
    case LDAP_INVALID_SYNTAX:
    case LDAP_NO_SUCH_OBJECT:
    case LDAP_ALIAS_PROBLEM:
    case LDAP_INVALID_DN_SYNTAX:
    case LDAP_IS_LEAF:
    case LDAP_ALIAS_DEREF_PROBLEM:
    case LDAP_FILTER_ERROR:
      return NSS_STATUS_NOTFOUND;
      break;
    case LDAP_SERVER_DOWN:
    case LDAP_TIMEOUT:
    case LDAP_UNAVAILABLE:
    case LDAP_BUSY:
#ifdef LDAP_CONNECT_ERROR
    case LDAP_CONNECT_ERROR:
#endif /* LDAP_CONNECT_ERROR */
    case LDAP_LOCAL_ERROR:
    case LDAP_INVALID_CREDENTIALS:
    default:
      return NSS_STATUS_UNAVAIL;
  }
}

static int do_sasl_interact(LDAP UNUSED(*ld),unsigned UNUSED(flags),void *defaults,void *_interact)
{
  char *authzid=(char *)defaults;
  sasl_interact_t *interact=(sasl_interact_t *)_interact;
  while (interact->id!=SASL_CB_LIST_END)
  {
    if (interact->id!=SASL_CB_USER)
      return LDAP_PARAM_ERROR;
    if (authzid!=NULL)
    {
      interact->result=authzid;
      interact->len=strlen(authzid);
    }
    else if (interact->defresult!=NULL)
    {
      interact->result=interact->defresult;
      interact->len=strlen(interact->defresult);
    }
    else
    {
      interact->result="";
      interact->len=0;
    }
    interact++;
  }
  return LDAP_SUCCESS;
}

/* this returns an LDAP result code */
static int do_bind(MYLDAP_SESSION *session)
{
  int rc;
  char *binddn,*bindarg;
  int usesasl;
  /*
   * If we're running as root, let us bind as a special
   * user, so we can fake shadow passwords.
   */
  /* TODO: store this information in the session */
  if ((geteuid()==0)&&(nslcd_cfg->ldc_rootbinddn!=NULL))
  {
    binddn=nslcd_cfg->ldc_rootbinddn;
    usesasl=nslcd_cfg->ldc_rootusesasl;
    bindarg=nslcd_cfg->ldc_rootusesasl?nslcd_cfg->ldc_rootsaslid:nslcd_cfg->ldc_rootbindpw;
  }
  else
  {
    binddn=nslcd_cfg->ldc_binddn;
    usesasl=nslcd_cfg->ldc_usesasl;
    bindarg=nslcd_cfg->ldc_usesasl?nslcd_cfg->ldc_saslid:nslcd_cfg->ldc_bindpw;
  }
  if (!usesasl)
  {
    /* do a simple bind */
    log_log(LOG_DEBUG,"simple bind as %s",binddn);
    rc=ldap_simple_bind_s(session->ls_conn,binddn,bindarg);
    if (rc!=LDAP_SUCCESS)
      log_log(LOG_ERR,"ldap_simple_bind_s() failed: %s: %s",ldap_err2string(rc),strerror(errno));
    return rc;
  }
  else
  {
    /* do a SASL bind */
    log_log(LOG_DEBUG,"SASL bind as %s",binddn);
    if (nslcd_cfg->ldc_sasl_secprops!=NULL)
    {
      rc=ldap_set_option(session->ls_conn,LDAP_OPT_X_SASL_SECPROPS,(void *)nslcd_cfg->ldc_sasl_secprops);
      if (rc!=LDAP_SUCCESS)
      {
        log_log(LOG_ERR,"unable to set SASL security properties: %s",ldap_err2string(rc));
        return -1;
      }
    }
    rc=ldap_sasl_interactive_bind_s(session->ls_conn,binddn,"GSSAPI",NULL,NULL,
                                    LDAP_SASL_QUIET,
                                    do_sasl_interact,(void *)bindarg);
    return rc;
  }
}

/*
 * This function is called by the LDAP library when chasing referrals.
 * It is configured with the ldap_set_rebind_proc() below.
 */
static int do_rebind(LDAP *UNUSED(ld),LDAP_CONST char UNUSED(*url),
                     ber_tag_t UNUSED(request),
                     ber_int_t UNUSED(msgid),void *arg)
{
  return do_bind((MYLDAP_SESSION *)arg);
}

/*
 * Close the global session, sending an unbind.
 * Closes connection to the LDAP server.
 */
static void do_close(MYLDAP_SESSION *session)
{
  log_log(LOG_DEBUG,"==> do_close");
  if (session->ls_conn!=NULL)
    ldap_unbind(session->ls_conn);
  session->ls_conn=NULL;
  log_log(LOG_DEBUG,"<== do_close");
}

static int do_ssl_options(void)
{
  /* TODO: save return value of ldap_set_option() and include it in the error message */
  /* rand file */
  if (nslcd_cfg->ldc_tls_randfile!=NULL)
  {
    if (ldap_set_option(NULL,LDAP_OPT_X_TLS_RANDOM_FILE,
                        nslcd_cfg->ldc_tls_randfile)!=LDAP_SUCCESS)
    {
      log_log(LOG_ERR,"setting of LDAP_OPT_X_TLS_RANDOM_FILE failed");
      return LDAP_OPERATIONS_ERROR;
    }
  }
  /* ca cert file */
  if (nslcd_cfg->ldc_tls_cacertfile!=NULL)
  {
    if (ldap_set_option(NULL,LDAP_OPT_X_TLS_CACERTFILE,
                        nslcd_cfg->ldc_tls_cacertfile)!=LDAP_SUCCESS)
    {
      log_log(LOG_ERR,"setting of LDAP_OPT_X_TLS_CACERTFILE failed");
      return LDAP_OPERATIONS_ERROR;
    }
  }
  /* ca cert directory */
  if (nslcd_cfg->ldc_tls_cacertdir!=NULL)
  {
    if (ldap_set_option(NULL,LDAP_OPT_X_TLS_CACERTDIR,
                        nslcd_cfg->ldc_tls_cacertdir)!=LDAP_SUCCESS)
    {
      log_log(LOG_ERR,"setting of LDAP_OPT_X_TLS_CACERTDIR failed");
      return LDAP_OPERATIONS_ERROR;
    }
  }
  /* require cert? */
  if (nslcd_cfg->ldc_tls_checkpeer > -1)
  {
    if (ldap_set_option(NULL,LDAP_OPT_X_TLS_REQUIRE_CERT,
                          &nslcd_cfg->ldc_tls_checkpeer)!=LDAP_SUCCESS)
    {
      log_log(LOG_ERR,"setting of LDAP_OPT_X_TLS_REQUIRE_CERT failed");
      return LDAP_OPERATIONS_ERROR;
    }
  }
  /* set cipher suite, certificate and private key: */
  if (nslcd_cfg->ldc_tls_ciphers != NULL)
  {
    if (ldap_set_option(NULL,LDAP_OPT_X_TLS_CIPHER_SUITE,
                          nslcd_cfg->ldc_tls_ciphers)!=LDAP_SUCCESS)
    {
      log_log(LOG_ERR,"setting of LDAP_OPT_X_TLS_CIPHER_SUITE failed");
      return LDAP_OPERATIONS_ERROR;
    }
  }

  if (nslcd_cfg->ldc_tls_cert != NULL)
  {
    if (ldap_set_option(NULL,LDAP_OPT_X_TLS_CERTFILE,
                        nslcd_cfg->ldc_tls_cert)!=LDAP_SUCCESS)
    {
      log_log(LOG_ERR,"setting of LDAP_OPT_X_TLS_CERTFILE failed");
      return LDAP_OPERATIONS_ERROR;
    }
  }
  if (nslcd_cfg->ldc_tls_key != NULL)
  {
    if (ldap_set_option(NULL,LDAP_OPT_X_TLS_KEYFILE,
                        nslcd_cfg->ldc_tls_key)!=LDAP_SUCCESS)
    {
      log_log(LOG_ERR,"setting of LDAP_OPT_X_TLS_KEYFILE failed");
      return LDAP_OPERATIONS_ERROR;
    }
  }
  return LDAP_SUCCESS;
}

/*
 * Opens connection to an LDAP server, sets all connection options
 * and binds to the server. This returns a simple (0/-1) status code.
 * TODO: this should return an LDAP error code
 */
static int do_open(MYLDAP_SESSION *session)
{
  struct timeval tv;
  int rc;
  time_t current_time;
  int sd=-1;
  /* check if the idle time for the connection has expired */
  if ((session->ls_conn!=NULL)&&nslcd_cfg->ldc_idle_timelimit)
  {
    time(&current_time);
    if ((session->ls_timestamp+nslcd_cfg->ldc_idle_timelimit)<current_time)
    {
      log_log(LOG_DEBUG,"do_open(): idle_timelimit reached");
      do_close(session);
    }
  }
  /* if the connection is still there (ie. do_close() wasn't
     called) then we can return the cached connection */
  if (session->ls_conn!=NULL)
  {
    return 0;
  }
  /* we should build a new session now */
  session->ls_conn=NULL;
  session->ls_timestamp=0;
  /* open the connection */
  rc=ldap_initialize(&(session->ls_conn),nslcd_cfg->ldc_uris[session->ls_current_uri]);
  if (rc!=LDAP_SUCCESS)
  {
    log_log(LOG_WARNING,"ldap_initialize(%s) failed: %s: %s",
                        nslcd_cfg->ldc_uris[session->ls_current_uri],
                        ldap_err2string(rc),strerror(errno));
    return -1;
  }
  else if (session->ls_conn==NULL)
  {
    log_log(LOG_WARNING,"ldap_initialize() returned NULL");
    return -1;
  }
  /* turn on debugging */
  if (nslcd_cfg->ldc_debug)
  {
    ber_set_option(NULL,LBER_OPT_DEBUG_LEVEL,&nslcd_cfg->ldc_debug);
    ldap_set_option(NULL,LDAP_OPT_DEBUG_LEVEL,&nslcd_cfg->ldc_debug);
  }
  /* the rebind function that is called when chasing referrals, see
     http://publib.boulder.ibm.com/infocenter/iseries/v5r3/topic/apis/ldap_set_rebind_proc.htm
     http://www.openldap.org/software/man.cgi?query=ldap_set_rebind_proc&manpath=OpenLDAP+2.4-Release */
  /* TODO: probably only set this if we should chase referrals */
  ldap_set_rebind_proc(session->ls_conn,do_rebind,session);
  /* set the protocol version to use */
  ldap_set_option(session->ls_conn,LDAP_OPT_PROTOCOL_VERSION,&nslcd_cfg->ldc_version);
  ldap_set_option(session->ls_conn,LDAP_OPT_DEREF,&nslcd_cfg->ldc_deref);
  ldap_set_option(session->ls_conn,LDAP_OPT_TIMELIMIT,&nslcd_cfg->ldc_timelimit);
  tv.tv_sec=nslcd_cfg->ldc_bind_timelimit;
  tv.tv_usec=0;
  ldap_set_option(session->ls_conn,LDAP_OPT_TIMEOUT,&tv);
  ldap_set_option(session->ls_conn,LDAP_OPT_NETWORK_TIMEOUT,&tv);
  ldap_set_option(session->ls_conn,LDAP_OPT_REFERRALS,nslcd_cfg->ldc_referrals?LDAP_OPT_ON:LDAP_OPT_OFF);
  ldap_set_option(session->ls_conn,LDAP_OPT_RESTART,nslcd_cfg->ldc_restart?LDAP_OPT_ON:LDAP_OPT_OFF);
  /* if SSL is desired, then enable it */
  if (nslcd_cfg->ldc_ssl_on==SSL_LDAPS)
  {
    int tls=LDAP_OPT_X_TLS_HARD;
    if (ldap_set_option(session->ls_conn,LDAP_OPT_X_TLS,&tls)!=LDAP_SUCCESS)
    {
      do_close(session);
      log_log(LOG_DEBUG,"<== do_open(TLS setup failed)");
      return -1;
    }
    /* set up SSL context */
    if (do_ssl_options()!=LDAP_SUCCESS)
    {
      do_close(session);
      log_log(LOG_DEBUG,"<== do_open(SSL setup failed)");
      return -1;
    }
  }
  /* bind to the server */
  rc=do_bind(session);
  if (rc!=LDAP_SUCCESS)
  {
    /* log actual LDAP error code */
    log_log(LOG_WARNING,"failed to bind to LDAP server %s: %s: %s",
            nslcd_cfg->ldc_uris[session->ls_current_uri],
            ldap_err2string(rc),strerror(errno));
    do_close(session);
    return -1;
  }
  /* disable keepalive on a LDAP connection socket */
  if (ldap_get_option(session->ls_conn,LDAP_OPT_DESC,&sd)==0)
  {
    int off=0;
    /* ignore errors */
    (void)setsockopt(sd,SOL_SOCKET,SO_KEEPALIVE,(void *)&off,sizeof(off));
    (void)fcntl(sd,F_SETFD,FD_CLOEXEC);
  }
  /* update last activity and finish off state */
  time(&(session->ls_timestamp));
  log_log(LOG_DEBUG,"do_open(): connected to %s",nslcd_cfg->ldc_uris[session->ls_current_uri]);
  return 0;
}

/*
 * Wrapper around ldap_result() to skip over search references
 * and deal transparently with the last entry.
 */
static enum nss_status do_result(MYLDAP_SEARCH *search)
{
  int rc=LDAP_UNAVAILABLE;
  enum nss_status stat=NSS_STATUS_TRYAGAIN;
  struct timeval tv,*tvp;
  int parserc;
  LDAPControl **resultControls;
  /* set up a timelimit value for operations */
  if (nslcd_cfg->ldc_timelimit==LDAP_NO_LIMIT)
    tvp=NULL;
  else
  {
    tv.tv_sec=nslcd_cfg->ldc_timelimit;
    tv.tv_usec=0;
    tvp=&tv;
  }
  /* loop until we have something else than a LDAP_RES_SEARCH_REFERENCE */
  do
  {
    /* free the previous message */
    if (search->msg!=NULL)
    {
      ldap_msgfree(search->msg);
      search->msg=NULL;
    }
    /* get the next result */
    rc=ldap_result(search->session->ls_conn,search->msgid,LDAP_MSG_ONE,tvp,&(search->msg));
    switch (rc)
    {
      case -1:
      case 0:
        if (ldap_get_option(search->session->ls_conn,LDAP_OPT_ERROR_NUMBER,&rc)!=LDAP_SUCCESS)
          rc=LDAP_UNAVAILABLE;
        log_log(LOG_ERR,"could not get LDAP result: %s",ldap_err2string(rc));
        stat=NSS_STATUS_UNAVAIL;
        break;
      case LDAP_RES_SEARCH_ENTRY:
        /* update timestamp on success */
        time(&(search->session->ls_timestamp));
        stat=NSS_STATUS_SUCCESS;
        break;
      case LDAP_RES_SEARCH_RESULT:
        resultControls=NULL;
        if (search->cookie!=NULL)
          ber_bvfree(search->cookie);
        search->cookie=NULL;
        /* NB: this frees search->msg */
        parserc=ldap_parse_result(search->session->ls_conn,search->msg,&rc,NULL,
                                  NULL,NULL,&resultControls,1);
        search->msg=NULL;
        if ((parserc!=LDAP_SUCCESS)&&(parserc!=LDAP_MORE_RESULTS_TO_RETURN))
        {
          stat = NSS_STATUS_UNAVAIL;
          ldap_abandon(search->session->ls_conn,search->msgid);
          log_log(LOG_ERR,"could not get LDAP result: %s",ldap_err2string(rc));
        }
        else if (resultControls!=NULL)
        {
          /* See if there are any more pages to come */
          parserc=ldap_parse_page_control(search->session->ls_conn,
                                          resultControls,NULL,
                                          &(search->cookie));
          /* TODO: handle the above return code?? */
          ldap_controls_free(resultControls);
          stat=NSS_STATUS_NOTFOUND;
        }
        else
          stat=NSS_STATUS_NOTFOUND;
        search->msgid=-1;
        break;
      default:
        stat = NSS_STATUS_UNAVAIL;
        break;
    }
  }
  while (rc==LDAP_RES_SEARCH_REFERENCE);
  /* TODO: check which statement could actually return LDAP_RES_SEARCH_REFERENCE
           and try to get as much error handling code outside of the loop */
  return stat;
}

static int do_search(MYLDAP_SEARCH *search,int *msgidp)
{
  int rc;
  LDAPControl *serverCtrls[2];
  LDAPControl **pServerCtrls;
  /* ensure that we have an open connection */
  if (do_open(search->session))
    return LDAP_SERVER_DOWN;
  /* if we're using paging, build a page control */
  if (nslcd_cfg->ldc_pagesize>0)
  {
    rc=ldap_create_page_control(search->session->ls_conn,nslcd_cfg->ldc_pagesize,
                                NULL,0,&serverCtrls[0]);
    if (rc!=LDAP_SUCCESS)
      return rc;
    serverCtrls[1]=NULL;
    pServerCtrls=serverCtrls;
  }
  else
    pServerCtrls=NULL;
  /* perform the search */
  rc=ldap_search_ext(search->session->ls_conn,search->base,search->scope,
                     search->filter,search->attrs,0,pServerCtrls,NULL,
                     LDAP_NO_LIMIT,LDAP_NO_LIMIT,msgidp);
  /* free the controls if we had them */
  if (pServerCtrls!=NULL)
  {
    ldap_control_free(serverCtrls[0]);
    serverCtrls[0]=NULL;
  }
  return rc;
}

/*
 * Function to call do_search() with reconnection logic (depending on
 * wheter res or msgid is not NULL).
 */
static enum nss_status do_with_reconnect(
        MYLDAP_SEARCH *search)
{
  int rc=LDAP_UNAVAILABLE, tries=0, backoff=0;
  int hard=1, start_uri=0, log=0;
  enum nss_status stat=NSS_STATUS_UNAVAIL;
  int msgid;
  int maxtries;
  /* get the maximum number of tries */
  maxtries=nslcd_cfg->ldc_reconnect_tries;
  /* keep trying until we have success or a hard failure */
  while ((stat==NSS_STATUS_UNAVAIL)&&(hard)&&(tries<maxtries))
  {
    /* sleep between tries */
    if (tries>0)
    {
      if (backoff==0)
        backoff=nslcd_cfg->ldc_reconnect_sleeptime;
      else if (backoff<nslcd_cfg->ldc_reconnect_maxsleeptime)
        backoff*=2;
      log_log(LOG_INFO,"reconnecting to LDAP server (sleeping %d seconds)...",backoff);
      (void)sleep(backoff);
    }
    /* for each "try", attempt to connect to all configured URIs */
    start_uri=search->session->ls_current_uri;
    do
    {
      stat=do_map_error(do_search(search,&msgid));
      /* if we got any feedback from the server, don't try any other URIs */
      if (stat!=NSS_STATUS_UNAVAIL)
        break;
      log++;
      /* try the next URI (with wrap-around) */
      search->session->ls_current_uri++;
      if (nslcd_cfg->ldc_uris[search->session->ls_current_uri]==NULL)
        search->session->ls_current_uri=0;
    }
    while (search->session->ls_current_uri != start_uri);
    /* if we had reachability problems with the server close the connection */
    /* TODO: we should probably close in the loop above */
    if (stat==NSS_STATUS_UNAVAIL)
    {
      do_close(search->session);
      /* If a soft reconnect policy is specified, then do not
       * try to reconnect to the LDAP server if it is down.
       */
      if (nslcd_cfg->ldc_reconnect_pol == LP_RECONNECT_SOFT)
        hard = 0;
      ++tries;
    }
  }

  switch (stat)
  {
    case NSS_STATUS_UNAVAIL:
      log_log(LOG_ERR,"could not search LDAP server - %s",ldap_err2string(rc));
      return NSS_STATUS_UNAVAIL;
    case NSS_STATUS_TRYAGAIN:
      log_log(LOG_ERR,"could not %s %sconnect to LDAP server - %s",
              hard?"hard":"soft", tries?"re":"",
              ldap_err2string(rc));
      return NSS_STATUS_UNAVAIL;
    case NSS_STATUS_SUCCESS:
      if (log)
      {
        char *uri=nslcd_cfg->ldc_uris[search->session->ls_current_uri];
        if (uri==NULL)
          uri = "(null)";
        if (tries)
          log_log(LOG_INFO,"reconnected to LDAP server %s after %d attempt%s",
            uri, tries,(tries == 1) ? "" : "s");
        else
          log_log(LOG_INFO,"reconnected to LDAP server %s", uri);
      }
      /* update the last activity on the connection */
      time(&(search->session->ls_timestamp));
      search->msgid=msgid;
      return NSS_STATUS_SUCCESS;
    case NSS_STATUS_NOTFOUND:
    case NSS_STATUS_RETURN:
    default:
      return stat;
  }
}

MYLDAP_SESSION *myldap_create_session(void)
{
  return myldap_session_new();
}

void myldap_session_cleanup(MYLDAP_SESSION *session)
{
  int i;
  /* go over all searches in the session and close them */
  for (i=0;i<MAX_SEARCHES_IN_SESSION;i++)
  {
    if (session->searches[i]!=NULL)
    {
      myldap_search_close(session->searches[i]);
      session->searches[i]=NULL;
    }
  }
}

MYLDAP_SEARCH *myldap_search(
        MYLDAP_SESSION *session,
        const char *base,int scope,const char *filter,const char **attrs)
{
  MYLDAP_SEARCH *search;
  int i;
  /* check parameters */
  if ((session==NULL)||(base==NULL)||(filter==NULL)||(attrs==NULL))
  {
    log_log(LOG_ERR,"myldap_search(): invalid parameter passed");
    errno=EINVAL;
    return NULL;
  }
  /* log the call */
  log_log(LOG_DEBUG,"myldap_search(base=\"%s\", filter=\"%s\")",
                    base,filter);
  /* allocate a new search entry */
  search=myldap_search_new(session,base,scope,filter,attrs);
  /* set up a new search */
  if (do_with_reconnect(search)!=NSS_STATUS_SUCCESS)
  {
    myldap_search_free(search);
    return NULL;
  }
  /* find a place in the session where we can register our search */
  for (i=0;(session->searches[i]!=NULL)&&(i<MAX_SEARCHES_IN_SESSION);i++)
    ;
  if (i>=MAX_SEARCHES_IN_SESSION)
  {
    log_log(LOG_ERR,"too many searches registered with session (max %d)",MAX_SEARCHES_IN_SESSION);
    myldap_search_free(search);
    return NULL;
  }
  /* regsiter search with the session so we can free it later on */
  session->searches[i]=search;
  return search;
}

void myldap_search_close(MYLDAP_SEARCH *search)
{
  int i;
  if ((search==NULL)||(search->session==NULL))
    return;
  /* abandon the search if there were more results to fetch */
  if ((search->msgid>-1)&&(do_result(search)==NSS_STATUS_SUCCESS))
    ldap_abandon(search->session->ls_conn,search->msgid);
  /* find the reference to this search in the session */
  for (i=0;i<MAX_SEARCHES_IN_SESSION;i++)
  {
    if (search->session->searches[i]==search)
      search->session->searches[i]=NULL;
  }
  /* free this search */
  myldap_search_free(search);
}

MYLDAP_ENTRY *myldap_get_entry(MYLDAP_SEARCH *search)
{
  enum nss_status stat=NSS_STATUS_SUCCESS;
  int msgid;
  int rc;
  /* check parameters */
  if ((search==NULL)||(search->session==NULL)||(search->session->ls_conn==NULL))
  {
    log_log(LOG_ERR,"myldap_get_entry(): invalid search entry passed");
    errno=EINVAL;
    return NULL;
  }
  /* if we have an existing result entry, free it */
  if (search->entry!=NULL)
  {
    myldap_entry_free(search->entry);
    search->entry=NULL;
  }
  /* try to parse results until we have a final error or ok */
  while (1)
  {
    /* get an entry from the LDAP server, the result
       is stored in context->ec_res */
    stat=do_result(search);
    /* we we have an entry construct a search entry from it */
    if (stat==NSS_STATUS_SUCCESS)
    {
      /* we have a normal entry, return it */
      search->entry=myldap_entry_new(search,search->msg);
      return search->entry;
    }
    else if ( (stat==NSS_STATUS_NOTFOUND) &&
              (search->cookie!=NULL) &&
              (search->cookie->bv_len!=0) )
    {
      /* we are using paged results, try the next page */
      LDAPControl *serverctrls[2]={ NULL, NULL };
      rc=ldap_create_page_control(search->session->ls_conn,
                                  nslcd_cfg->ldc_pagesize,
                                  search->cookie,0,&serverctrls[0]);
      if (rc!=LDAP_SUCCESS)
      {
        log_log(LOG_WARNING,"myldap_get_entry(): ldap_create_page_control() failed: %s",
                            ldap_err2string(rc));
        /* FIXME: figure out if we need to free something */
        return NULL;
      }
      rc=ldap_search_ext(search->session->ls_conn,
                         search->base,search->scope,search->filter,
                         search->attrs,0,serverctrls,NULL,LDAP_NO_LIMIT,
                         LDAP_NO_LIMIT,&msgid);
      ldap_control_free(serverctrls[0]);
      if (msgid<0)
      {
        log_log(LOG_WARNING,"myldap_get_entry(): ldap_search_ext() failed: %s",
                            ldap_err2string(rc));
        /* FIXME: figure out if we need to free something */
        return NULL;
      }
      search->msgid=msgid;
      /* we continue with another pass */
    }
    else
    {
      log_log(LOG_DEBUG,"myldap_get_entry(): do_result() returned error code");
      /* there was another problem, bail out */
      return NULL;
    }
  }
}

/*
 * Get the DN from the entry. This function only returns NULL (and sets
 * errno) if an incorrect entry is passed. If the DN value cannot be
 * retreived "unknown" is returned instead.
 */
const char *myldap_get_dn(MYLDAP_ENTRY *entry)
{
  int rc;
  /* check parameters */
  if ((entry==NULL)||(entry->search==NULL)||(entry->search->session==NULL)||
      (entry->search->session->ls_conn==NULL)||(entry->msg==NULL))
  {
    log_log(LOG_ERR,"myldap_get_dn(): invalid result entry passed");
    errno=EINVAL;
    return NULL;
  }
  /* if we don't have it yet, retreive it */
  if (entry->dn==NULL)
  {
    entry->dn=ldap_get_dn(entry->search->session->ls_conn,entry->msg);
    if (entry->dn==NULL)
    {
      if (ldap_get_option(entry->search->session->ls_conn,LDAP_OPT_ERROR_NUMBER,&rc)!=LDAP_SUCCESS)
        rc=LDAP_UNAVAILABLE;
      log_log(LOG_WARNING,"ldap_get_dn() returned NULL: %s",ldap_err2string(rc));
    }
  }
  /* if we still don't have it, return unknown */
  if (entry->dn==NULL)
    return "unknown";
  /* return it */
  return entry->dn;
}

/* Simple wrapper around ldap_get_values(). */
const char **myldap_get_values(MYLDAP_ENTRY *entry,const char *attr)
{
  char **values;
  int rc;
  /* check parameters */
  if ((entry==NULL)||(entry->search==NULL)||(entry->search->session==NULL)||
      (entry->search->session->ls_conn==NULL)||(entry->msg==NULL))
  {
    log_log(LOG_ERR,"myldap_get_values(): invalid result entry passed");
    errno=EINVAL;
    return NULL;
  }
  else if (attr==NULL)
  {
    log_log(LOG_ERR,"myldap_get_values(): invalid attribute name passed");
    errno=EINVAL;
    return NULL;
  }
  /* get the values from the cache */
  values=(char **)dict_get(entry->attributevalues,attr);
  if (values==NULL)
  {
    /* cache miss, get from LDAP */
    values=ldap_get_values(entry->search->session->ls_conn,entry->msg,attr);
    if (values==NULL)
    {
      if (ldap_get_option(entry->search->session->ls_conn,LDAP_OPT_ERROR_NUMBER,&rc)!=LDAP_SUCCESS)
        rc=LDAP_UNAVAILABLE;
      /* ignore decoding errors as they are just nonexisting attribute values */
      if (rc!=LDAP_DECODING_ERROR)
        log_log(LOG_WARNING,"myldap_get_values(): ldap_get_values() returned NULL: %s",ldap_err2string(rc));
    }
    /* store values entry so we can free it later on */
    if (values!=NULL)
      dict_put(entry->attributevalues,attr,values);
  }
  return (const char **)values;
}

/* Go over the entries in exploded_rdn and see if any start with
   the requested attribute. Return a reference to the value part of
   the DN (does not modify exploded_rdn). */
static const char *find_rdn_value(char **exploded_rdn,const char *attr)
{
  int i,j;
  int l;
  if (exploded_rdn==NULL)
    return NULL;
  /* go over all RDNs */
  l=strlen(attr);
  for (i=0;exploded_rdn[i]!=NULL;i++)
  {
    /* check that RDN starts with attr */
    if (strncasecmp(exploded_rdn[i],attr,l)!=0)
      continue;
    /* skip spaces */
    for (j=l;isspace(exploded_rdn[i][j]);j++)
      /* nothing here */;
    /* ensure that we found an equals sign now */
    if (exploded_rdn[i][j]!='=')
    j++;
    /* skip more spaces */
    for (j++;isspace(exploded_rdn[i][j]);j++)
      /* nothing here */;
    /* ensure that we're not at the end of the string */
    if (exploded_rdn[i][j]=='\0')
      continue;
    /* we found our value */
    return exploded_rdn[i]+j;
  }
  /* fail */
  return NULL;
}

const char *myldap_get_rdn_value(MYLDAP_ENTRY *entry,const char *attr)
{
  const char *dn;
  char **exploded_dn;
  /* check parameters */
  if ((entry==NULL)||(entry->search==NULL)||(entry->search->session==NULL)||
      (entry->search->session->ls_conn==NULL)||(entry->msg==NULL))
  {
    log_log(LOG_ERR,"myldap_get_rdn_value(): invalid result entry passed");
    errno=EINVAL;
    return NULL;
  }
  else if (attr==NULL)
  {
    log_log(LOG_ERR,"myldap_get_rdn_value(): invalid attribute name passed");
    errno=EINVAL;
    return NULL;
  }
  /* check if entry contains exploded_rdn */
  if (entry->exploded_rdn==NULL)
  {
    /* check if we have a DN */
    dn=myldap_get_dn(entry);
    if (dn==NULL)
      return NULL;
    /* explode dn into { "uid=test", "ou=people", ..., NULL } */
    exploded_dn=ldap_explode_dn(dn,0);
    if ((exploded_dn==NULL)||(exploded_dn[0]==NULL))
    {
      log_log(LOG_WARNING,"myldap_get_rdn_value(): ldap_explode_dn(%s) returned NULL: %s",
                          dn,strerror(errno));
      return NULL;
    }
    /* explode rdn (first part of exploded_dn),
        e.g. "cn=Test User+uid=testusr" into
       { "cn=Test User", "uid=testusr", NULL } */
    entry->exploded_rdn=ldap_explode_rdn(exploded_dn[0],0);
    ldap_value_free(exploded_dn);
  }
  /* find rnd value */
  return find_rdn_value(entry->exploded_rdn,attr);
}

int myldap_has_objectclass(MYLDAP_ENTRY *entry,const char *objectclass)
{
  const char **values;
  int i;
  if ((entry==NULL)||(objectclass==NULL))
  {
    log_log(LOG_ERR,"myldap_has_objectclass(): invalid argument passed");
    errno=EINVAL;
    return 0;
  }
  values=myldap_get_values(entry,"objectClass");
  if (values==NULL)
  {
    log_log(LOG_ERR,"myldap_has_objectclass(): myldap_get_values() returned NULL");
    return 0;
  }
  for (i=0;values[i]!=NULL;i++)
  {
    if (strcasecmp(values[i],objectclass)==0)
      return -1;
  }
  return 0;
}

int myldap_escape(const char *src,char *buffer,size_t buflen)
{
  size_t pos=0;
  /* go over all characters in source string */
  for (;*src!='\0';src++)
  {
    /* check if char will fit */
    if (pos>=(buflen+4))
      return -1;
    /* do escaping for some characters */
    switch (*src)
    {
      case '*':
        strcpy(buffer+pos,"\\2a");
        pos+=3;
        break;
      case '(':
        strcpy(buffer+pos,"\\28");
        pos+=3;
        break;
      case ')':
        strcpy(buffer+pos,"\\29");
        pos+=3;
        break;
      case '\\':
        strcpy(buffer+pos,"\\5c");
        pos+=3;
        break;
      default:
        /* just copy character */
        buffer[pos++]=*src;
        break;
    }
  }
  /* terminate destination string */
  buffer[pos]='\0';
  return 0;
}