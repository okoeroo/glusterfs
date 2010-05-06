/*
  Copyright (c) 2010 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "rpcsvc.h"
#include "rpc-socket.h"
#include "dict.h"
#include "logging.h"
#include "byte-order.h"
#include "common-utils.h"
#include "compat-errno.h"
#include "list.h"
#include "xdr-rpc.h"
#include "iobuf.h"
#include "globals.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <arpa/inet.h>
#include <rpc/xdr.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>


#define rpcsvc_alloc_request(con, request)                              \
        do {                                                            \
                request = (rpcsvc_request_t *) mem_get ((con)->rxpool); \
                memset (request, 0, sizeof (rpcsvc_request_t));         \
        } while (0)                                                     \

/* The generic event handler for every stage */
void *
rpcsvc_stage_proc (void *arg)
{
        rpcsvc_stage_t          *stg = (rpcsvc_stage_t *)arg;

        if (!stg)
                return NULL;

        event_dispatch (stg->eventpool);
        return NULL;
}


rpcsvc_stage_t *
rpcsvc_stage_init (rpcsvc_t *svc)
{
        rpcsvc_stage_t          *stg = NULL;
        int                     ret = -1;
        size_t                  stacksize = RPCSVC_THREAD_STACK_SIZE;
        pthread_attr_t          stgattr;
        unsigned int            eventpoolsize = 0;

        if (!svc)
                return NULL;

        stg = GF_CALLOC (1, sizeof(*stg), gf_common_mt_rpcsvc_stage_t);
        if (!stg)
                return NULL;

        eventpoolsize = svc->memfactor * RPCSVC_EVENTPOOL_SIZE_MULT;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "event pool size: %d", eventpoolsize);
        stg->eventpool = event_pool_new (eventpoolsize);
        if (!stg->eventpool)
                goto free_stg;

        pthread_attr_init (&stgattr);
        ret = pthread_attr_setstacksize (&stgattr, stacksize);
        if (ret == EINVAL)
                gf_log (GF_RPCSVC, GF_LOG_WARNING,
                                "Using default thread stack size");

        ret = pthread_create (&stg->tid, &stgattr, rpcsvc_stage_proc,
                              (void *)stg);
        if (ret != 0) {
                ret = -1;
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Stage creation failed");
                goto free_stg;
        }

        stg->svc = svc;
        ret = 0;
free_stg:
        if (ret == -1) {
                GF_FREE (stg);
                stg = NULL;
        }

        return stg;
}


int
rpcsvc_init_options (rpcsvc_t *svc, dict_t *options)
{
        svc->memfactor = RPCSVC_DEFAULT_MEMFACTOR;
        return 0;
}


/* The global RPC service initializer.
 * Starts up the stages and then waits for RPC program registrations
 * to come in.
 */
rpcsvc_t *
rpcsvc_init (glusterfs_ctx_t *ctx, dict_t *options)
{
        rpcsvc_t        *svc = NULL;
        int             ret = -1;

        if ((!ctx) || (!options))
                return NULL;

        svc = GF_CALLOC (1, sizeof (*svc), gf_common_mt_rpcsvc_t);
        if (!svc)
                return NULL;

        pthread_mutex_init (&svc->rpclock, NULL);
        INIT_LIST_HEAD (&svc->stages);
        INIT_LIST_HEAD (&svc->authschemes);

        ret = rpcsvc_init_options (svc, options);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to init options");
                goto free_svc;
        }

        ret = rpcsvc_auth_init (svc, options);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to init "
                        "authentication");
                goto free_svc;
        }

        ret = -1;
        svc->defaultstage = rpcsvc_stage_init (svc);
        if (!svc->defaultstage) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR,"RPC service init failed.");
                goto free_svc;
        }
        svc->options = options;
        svc->ctx = ctx;
        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "RPC service inited.");

        ret = 0;
free_svc:
        if (ret == -1) {
                GF_FREE (svc);
                svc = NULL;
        }

        return svc;
}


/* Once multi-threaded support is complete, we'll be able to round-robin
 * the various incoming connections over the many available stages. This
 * function selects one from among all the stages.
 */
rpcsvc_stage_t *
rpcsvc_select_stage (rpcsvc_t *rpcservice)
{
        if (!rpcservice)
                return NULL;

        return rpcservice->defaultstage;
}


int
rpcsvc_conn_peer_check_search (dict_t *options, char *pattern, char *clstr)
{
        int                     ret = -1;
        char                    *addrtok = NULL;
        char                    *addrstr = NULL;
        char                    *svptr = NULL;

        if ((!options) || (!clstr))
                return -1;

        if (!dict_get (options, pattern))
                return -1;

        ret = dict_get_str (options, pattern, &addrstr);
        if (ret < 0) {
                ret = -1;
                goto err;
        }

        if (!addrstr) {
                ret = -1;
                goto err;
        }

        addrtok = strtok_r (addrstr, ",", &svptr);
        while (addrtok) {

                ret = fnmatch (addrtok, clstr, FNM_CASEFOLD);
                if (ret == 0)
                        goto err;

                addrtok = strtok_r (NULL, ",", &svptr);
        }

        ret = -1;
err:

        return ret;
}


int
rpcsvc_conn_peer_check_allow (dict_t *options, char *volname, char *clstr)
{
        int     ret = RPCSVC_AUTH_DONTCARE;
        char    *srchstr = NULL;
        char    globalrule[] = "rpc-auth.addr.allow";

        if ((!options) || (!clstr))
                return ret;

        /* If volname is NULL, then we're searching for the general rule to
         * determine the current address in clstr is allowed or not for all
         * subvolumes.
         */
        if (volname) {
                ret = gf_asprintf (&srchstr, "rpc-auth.addr.%s.allow", volname);
                if (ret == -1) {
                        gf_log (GF_RPCSVC, GF_LOG_ERROR, "asprintf failed");
                        ret = RPCSVC_AUTH_DONTCARE;
                        goto out;
                }
        } else
                srchstr = globalrule;

        ret = rpcsvc_conn_peer_check_search (options, srchstr, clstr);
        if (volname)
                GF_FREE (srchstr);

        if (ret == 0)
                ret = RPCSVC_AUTH_ACCEPT;
        else
                ret = RPCSVC_AUTH_DONTCARE;
out:
        return ret;
}

int
rpcsvc_conn_peer_check_reject (dict_t *options, char *volname, char *clstr)
{
        int     ret = RPCSVC_AUTH_DONTCARE;
        char    *srchstr = NULL;
        char    generalrule[] = "rpc-auth.addr.reject";

        if ((!options) || (!clstr))
                return ret;

        if (volname) {
                ret = gf_asprintf (&srchstr, "rpc-auth.addr.%s.reject", volname);
                if (ret == -1) {
                        gf_log (GF_RPCSVC, GF_LOG_ERROR, "asprintf failed");
                        ret = RPCSVC_AUTH_REJECT;
                        goto out;
                }
        } else
                srchstr = generalrule;

        ret = rpcsvc_conn_peer_check_search (options, srchstr, clstr);
        if (volname)
                GF_FREE (srchstr);

        if (ret == 0)
                ret = RPCSVC_AUTH_REJECT;
        else
                ret = RPCSVC_AUTH_DONTCARE;
out:
        return ret;
}


/* This function tests the results of the allow rule and the reject rule to
 * combine them into a single result that can be used to determine if the
 * connection should be allowed to proceed.
 * Heres the test matrix we need to follow in this function.
 *
 * A -  Allow, the result of the allow test. Never returns R.
 * R - Reject, result of the reject test. Never returns A.
 * Both can return D or dont care if no rule was given.
 *
 * | @allow | @reject | Result |
 * |    A   |   R     | R      |
 * |    D   |   D     | D      |
 * |    A   |   D     | A      |
 * |    D   |   R     | R      |
 */
int
rpcsvc_combine_allow_reject_volume_check (int allow, int reject)
{
        int     final = RPCSVC_AUTH_REJECT;

        /* If allowed rule allows but reject rule rejects, we stay cautious
         * and reject. */
        if ((allow == RPCSVC_AUTH_ACCEPT) && (reject == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;
        /* if both are dont care, that is user did not specify for either allow
         * or reject, we leave it up to the general rule to apply, in the hope
         * that there is one.
         */
        else if ((allow == RPCSVC_AUTH_DONTCARE) &&
                 (reject == RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_DONTCARE;
        /* If one is dont care, the other one applies. */
        else if ((allow == RPCSVC_AUTH_ACCEPT) &&
                 (reject == RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((allow == RPCSVC_AUTH_DONTCARE) &&
                 (reject == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;

        return final;
}


/* Combines the result of the general rule test against, the specific rule
 * to determine final permission for the client's address.
 *
 * | @gen   | @spec   | Result |
 * |    A   |   A     | A      |
 * |    A   |   R     | R      |
 * |    A   |   D     | A      |
 * |    D   |   A     | A      |
 * |    D   |   R     | R      |
 * |    D   |   D     | D      |
 * |    R   |   A     | A      |
 * |    R   |   D     | R      |
 * |    R   |   R     | R      |
 */
int
rpcsvc_combine_gen_spec_addr_checks (int gen, int spec)
{
        int     final = RPCSVC_AUTH_REJECT;

        if ((gen == RPCSVC_AUTH_ACCEPT) && (spec == RPCSVC_AUTH_ACCEPT))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_ACCEPT) && (spec == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;
        else if ((gen == RPCSVC_AUTH_ACCEPT) && (spec == RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_DONTCARE) && (spec == RPCSVC_AUTH_ACCEPT))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_DONTCARE) && (spec == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;
        else if ((gen == RPCSVC_AUTH_DONTCARE) && (spec== RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_DONTCARE;
        else if ((gen == RPCSVC_AUTH_REJECT) && (spec == RPCSVC_AUTH_ACCEPT))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_REJECT) && (spec == RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_REJECT;
        else if ((gen == RPCSVC_AUTH_REJECT) && (spec == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;

        return final;
}



/* Combines the result of the general rule test against, the specific rule
 * to determine final test for the connection coming in for a given volume.
 *
 * | @gen   | @spec   | Result |
 * |    A   |   A     | A      |
 * |    A   |   R     | R      |
 * |    A   |   D     | A      |
 * |    D   |   A     | A      |
 * |    D   |   R     | R      |
 * |    D   |   D     | R      |, special case, we intentionally disallow this.
 * |    R   |   A     | A      |
 * |    R   |   D     | R      |
 * |    R   |   R     | R      |
 */
int
rpcsvc_combine_gen_spec_volume_checks (int gen, int spec)
{
        int     final = RPCSVC_AUTH_REJECT;

        if ((gen == RPCSVC_AUTH_ACCEPT) && (spec == RPCSVC_AUTH_ACCEPT))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_ACCEPT) && (spec == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;
        else if ((gen == RPCSVC_AUTH_ACCEPT) && (spec == RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_DONTCARE) && (spec == RPCSVC_AUTH_ACCEPT))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_DONTCARE) && (spec == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;
        /* On no rule, we reject. */
        else if ((gen == RPCSVC_AUTH_DONTCARE) && (spec== RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_REJECT;
        else if ((gen == RPCSVC_AUTH_REJECT) && (spec == RPCSVC_AUTH_ACCEPT))
                final = RPCSVC_AUTH_ACCEPT;
        else if ((gen == RPCSVC_AUTH_REJECT) && (spec == RPCSVC_AUTH_DONTCARE))
                final = RPCSVC_AUTH_REJECT;
        else if ((gen == RPCSVC_AUTH_REJECT) && (spec == RPCSVC_AUTH_REJECT))
                final = RPCSVC_AUTH_REJECT;

        return final;
}


int
rpcsvc_conn_peer_check_name (dict_t *options, char *volname,
                             rpcsvc_conn_t *conn)
{
        int     ret = RPCSVC_AUTH_REJECT;
        int     aret = RPCSVC_AUTH_REJECT;
        int     rjret = RPCSVC_AUTH_REJECT;
        char    clstr[RPCSVC_PEER_STRLEN];

        if (!conn)
                return ret;

        ret = rpcsvc_conn_peername (conn, clstr, RPCSVC_PEER_STRLEN);
        if (ret != 0) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to get remote addr: "
                        "%s", gai_strerror (ret));
                ret = RPCSVC_AUTH_REJECT;
                goto err;
        }

        aret = rpcsvc_conn_peer_check_allow (options, volname, clstr);
        rjret = rpcsvc_conn_peer_check_reject (options, volname, clstr);

        ret = rpcsvc_combine_allow_reject_volume_check (aret, rjret);

err:
        return ret;
}


int
rpcsvc_conn_peer_check_addr (dict_t *options, char *volname,rpcsvc_conn_t *conn)
{
        int     ret = RPCSVC_AUTH_REJECT;
        int     aret = RPCSVC_AUTH_DONTCARE;
        int     rjret = RPCSVC_AUTH_REJECT;
        char    clstr[RPCSVC_PEER_STRLEN];

        if (!conn)
                return ret;

        ret = rpcsvc_conn_peeraddr (conn, clstr, RPCSVC_PEER_STRLEN, NULL, 0);
        if (ret != 0) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to get remote addr: "
                        "%s", gai_strerror (ret));
                ret = RPCSVC_AUTH_REJECT;
                goto err;
        }

        aret = rpcsvc_conn_peer_check_allow (options, volname, clstr);
        rjret = rpcsvc_conn_peer_check_reject (options, volname, clstr);

        ret = rpcsvc_combine_allow_reject_volume_check (aret, rjret);
err:
        return ret;
}


int
rpcsvc_conn_check_volume_specific (dict_t *options, char *volname,
                                   rpcsvc_conn_t *conn)
{
        int             namechk = RPCSVC_AUTH_REJECT;
        int             addrchk = RPCSVC_AUTH_REJECT;
        gf_boolean_t    namelookup = _gf_true;
        char            *namestr = NULL;
        int             ret = 0;

        if ((!options) || (!volname) || (!conn))
                return RPCSVC_AUTH_REJECT;

        /* Enabled by default */
        if ((dict_get (options, "rpc-auth.addr.namelookup"))) {
                ret = dict_get_str (options, "rpc-auth.addr.namelookup"
                                    , &namestr);
                if (ret == 0)
                        ret = gf_string2boolean (namestr, &namelookup);
        }

        /* We need two separate checks because the rules with addresses in them
         * can be network addresses which can be general and names can be
         * specific which will over-ride the network address rules.
         */
        if (namelookup)
                namechk = rpcsvc_conn_peer_check_name (options, volname, conn);
        addrchk = rpcsvc_conn_peer_check_addr (options, volname, conn);

        if (namelookup)
                ret = rpcsvc_combine_gen_spec_addr_checks (addrchk, namechk);
        else
                ret = addrchk;

        return ret;
}


int
rpcsvc_conn_check_volume_general (dict_t *options, rpcsvc_conn_t *conn)
{
        int             addrchk = RPCSVC_AUTH_REJECT;
        int             namechk = RPCSVC_AUTH_REJECT;
        gf_boolean_t    namelookup = _gf_true;
        char            *namestr = NULL;
        int             ret = 0;

        if ((!options) || (!conn))
                return RPCSVC_AUTH_REJECT;

        /* Enabled by default */
        if ((dict_get (options, "rpc-auth.addr.namelookup"))) {
                ret = dict_get_str (options, "rpc-auth.addr.namelookup"
                                    , &namestr);
                if (ret == 0)
                        ret = gf_string2boolean (namestr, &namelookup);
        }

        /* We need two separate checks because the rules with addresses in them
         * can be network addresses which can be general and names can be
         * specific which will over-ride the network address rules.
         */
        if (namelookup)
                namechk = rpcsvc_conn_peer_check_name (options, NULL, conn);
        addrchk = rpcsvc_conn_peer_check_addr (options, NULL, conn);

        if (namelookup)
                ret = rpcsvc_combine_gen_spec_addr_checks (addrchk, namechk);
        else
                ret = addrchk;

        return ret;
}

int
rpcsvc_conn_peer_check (dict_t *options, char *volname, rpcsvc_conn_t *conn)
{
        int     general_chk = RPCSVC_AUTH_REJECT;
        int     specific_chk = RPCSVC_AUTH_REJECT;

        if ((!options) || (!volname) || (!conn))
                return RPCSVC_AUTH_REJECT;

        general_chk = rpcsvc_conn_check_volume_general (options, conn);
        specific_chk = rpcsvc_conn_check_volume_specific (options, volname,
                                                          conn);

        return rpcsvc_combine_gen_spec_volume_checks (general_chk,specific_chk);
}


char *
rpcsvc_volume_allowed (dict_t *options, char *volname)
{
        char    globalrule[] = "rpc-auth.addr.allow";
        char    *srchstr = NULL;
        char    *addrstr = NULL;
        int     ret = -1;

        if ((!options) || (!volname))
                return NULL;

        ret = gf_asprintf (&srchstr, "rpc-auth.addr.%s.allow", volname);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "asprintf failed");
                goto out;
        }

        if (!dict_get (options, srchstr)) {
                GF_FREE (srchstr);
                srchstr = globalrule;
                ret = dict_get_str (options, srchstr, &addrstr);
        } else
                ret = dict_get_str (options, srchstr, &addrstr);

out:
        return addrstr;
}


/* Initialize the core of a connection */
rpcsvc_conn_t *
rpcsvc_conn_init (rpcsvc_t *svc, rpcsvc_program_t *prog, int sockfd)
{
        rpcsvc_conn_t  *conn = NULL;
        int             ret = -1;
        unsigned int    poolcount = 0;

        conn = GF_CALLOC (1, sizeof(*conn), gf_common_mt_rpcsvc_conn_t);
        if (!conn) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "memory allocation failed");
                return NULL;
        }

        conn->sockfd = sockfd;
        conn->program = (void *)prog;
        INIT_LIST_HEAD (&conn->txbufs);
        poolcount = RPCSVC_POOLCOUNT_MULT * svc->memfactor;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "tx pool: %d", poolcount);
        conn->txpool = mem_pool_new (rpcsvc_txbuf_t, poolcount);
        if (!conn->txpool) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "mem pool allocation failed");
                goto free_conn;
        }

        gf_log (GF_RPCSVC, GF_LOG_TRACE, "rx pool: %d", poolcount);
        conn->rxpool = mem_pool_new (rpcsvc_request_t, poolcount);
        if (!conn->rxpool) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "mem pool allocation failed");
                goto free_txp;
        }

        /* Cannot consider a connection connected unless the user of this
         * connection decides it is ready to use. It is possible that we have
         * to free this connection soon after. That free will not happpen
         * unless the state is disconnected.
         */
        conn->connstate = RPCSVC_CONNSTATE_DISCONNECTED;
        pthread_mutex_init (&conn->connlock, NULL);
        conn->connref = 0;
        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "New connection inited: sockfd: %d",
                sockfd);

        ret = 0;
free_txp:
        if (ret == -1)
                mem_pool_destroy (conn->txpool);

free_conn:
        if (ret == -1) {
                GF_FREE (conn);
                conn = NULL;
        }

        return conn;
}


void
rpcsvc_conn_destroy (rpcsvc_conn_t *conn)
{
        mem_pool_destroy (conn->txpool);
        mem_pool_destroy (conn->rxpool);

        if (conn->program->conn_destroy)
                conn->program->conn_destroy (conn->program->private, conn);

        /* Need to destory record state, txlists etc. */
        GF_FREE (conn);
        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "Connection destroyed");
}


int
__rpcsvc_conn_unref (rpcsvc_conn_t *conn)
{
        --conn->connref;
        return conn->connref;
}


void
__rpcsvc_conn_deinit (rpcsvc_conn_t *conn)
{
        if (!conn)
                return;

        if ((conn->stage) && (conn->stage->eventpool)) {
                event_unregister (conn->stage->eventpool, conn->sockfd,
                                  conn->eventidx);
        }

        if (rpcsvc_conn_check_active (conn)) {
                gf_log (GF_RPCSVC, GF_LOG_DEBUG, "Connection de-activated:"
                        " sockfd: %d", conn->sockfd);
                conn->connstate = RPCSVC_CONNSTATE_DISCONNECTED;
        }

        if (conn->sockfd != -1) {
                close (conn->sockfd);
                conn->sockfd = -1;
        }
}


void
rpcsvc_conn_deinit (rpcsvc_conn_t *conn)
{
        int ref = 0;

        if (!conn)
                return;

        pthread_mutex_lock (&conn->connlock);
        {
                __rpcsvc_conn_deinit (conn);
                ref = __rpcsvc_conn_unref (conn);
        }
        pthread_mutex_unlock (&conn->connlock);

        if (ref == 0)
                rpcsvc_conn_destroy (conn);

        return;
}


void
rpcsvc_conn_unref (rpcsvc_conn_t *conn)
{
        int ref = 0;
        if (!conn)
                return;

        pthread_mutex_lock (&conn->connlock);
        {
                ref = __rpcsvc_conn_unref (conn);
        }
        pthread_mutex_unlock (&conn->connlock);

        if (ref == 0)
                rpcsvc_conn_destroy (conn);
}


int
rpcsvc_conn_active (rpcsvc_conn_t *conn)
{
        int     status = 0;

        if (!conn)
                return 0;

        pthread_mutex_lock (&conn->connlock);
        {
                status = rpcsvc_conn_check_active (conn);
        }
        pthread_mutex_unlock (&conn->connlock);

        return status;
}



void
rpcsvc_conn_ref (rpcsvc_conn_t *conn)
{
        if (!conn)
                return;

        pthread_mutex_lock (&conn->connlock);
        {
                ++conn->connref;
        }
        pthread_mutex_unlock (&conn->connlock);

        return;
}


void
rpcsvc_conn_state_init (rpcsvc_conn_t *conn)
{
        if (!conn)
                return;

        ++conn->connref;
        conn->connstate = RPCSVC_CONNSTATE_CONNECTED;
}

/* Builds a rpcsvc_conn_t with the aim of listening on it.
 */
rpcsvc_conn_t *
rpcsvc_conn_listen_init (rpcsvc_t *svc, rpcsvc_program_t *newprog)
{
        rpcsvc_conn_t  *conn = NULL;
        int             sock = -1;

        if (!newprog)
                return NULL;

        sock = rpcsvc_socket_listen (newprog->progaddrfamily, newprog->proghost,
                                     newprog->progport);
        if (sock == -1)
                goto err;

        conn = rpcsvc_conn_init (svc, newprog, sock);
        if (!conn)
                goto sock_close_err;

        rpcsvc_conn_state_init (conn);
sock_close_err:
        if (!conn)
                close (sock);

err:
        return conn;
}

void
rpcsvc_record_init (rpcsvc_record_state_t *rs, struct iobuf_pool *pool)
{
        if (!rs)
                return;

        rs->state = RPCSVC_READ_FRAGHDR;
        rs->vecstate = 0;
        rs->remainingfraghdr = RPCSVC_FRAGHDR_SIZE;
        rs->remainingfrag = 0;
        rs->fragsize = 0;
        rs->recordsize = 0;
        rs->islastfrag = 0;

        /* If the rs preserves a ref to the iob used by the previous request,
         * we must unref it here to prevent memory leak.
         * If program actor wanted to keep that memory around, it should've
         * refd it on entry into the actor.
         */
        if (rs->activeiob)
                iobuf_unref (rs->activeiob);

        if (rs->vectoriob) {
                iobuf_unref (rs->vectoriob);
                rs->vectoriob = NULL;
        }

        rs->activeiob = iobuf_get (pool);
        rs->fragcurrent = iobuf_ptr (rs->activeiob);

        memset (rs->fragheader, 0, RPCSVC_FRAGHDR_SIZE);
        rs->hdrcurrent = &rs->fragheader[0];

}


int
rpcsvc_conn_privport_check (rpcsvc_t *svc, char *volname, rpcsvc_conn_t *conn)
{
        struct sockaddr_in      sa;
        int                     ret = RPCSVC_AUTH_REJECT;
        socklen_t               sasize = sizeof (sa);
        char                    *srchstr = NULL;
        char                    *valstr = NULL;
        int                     globalinsecure = RPCSVC_AUTH_REJECT;
        int                     exportinsecure = RPCSVC_AUTH_DONTCARE;
        uint16_t                port = 0;
        gf_boolean_t            insecure = _gf_false;

        if ((!svc) || (!volname) || (!conn))
                return ret;

        ret = rpcsvc_conn_peeraddr (conn, NULL, 0, (struct sockaddr *)&sa,
                                    sasize);
        if (ret != 0) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to get peer addr: %s",
                        gai_strerror (ret));
                ret = RPCSVC_AUTH_REJECT;
                goto err;
        }

        port = ntohs (sa.sin_port);
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Client port: %d", (int)port);
        /* If the port is already a privileged one, dont bother with checking
         * options.
         */
        if (port <= 1024) {
                ret = RPCSVC_AUTH_ACCEPT;
                goto err;
        }

        /* Disabled by default */
        if ((dict_get (svc->options, "rpc-auth.ports.insecure"))) {
                ret = dict_get_str (svc->options, "rpc-auth.ports.insecure"
                                    , &srchstr);
                if (ret == 0) {
                        ret = gf_string2boolean (srchstr, &insecure);
                        if (ret == 0) {
                                if (insecure == _gf_true)
                                        globalinsecure = RPCSVC_AUTH_ACCEPT;
                        } else
                                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to"
                                        " read rpc-auth.ports.insecure value");
                } else
                        gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to"
                                " read rpc-auth.ports.insecure value");
        }

        /* Disabled by default */
        ret = gf_asprintf (&srchstr, "rpc-auth.ports.%s.insecure", volname);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "asprintf failed");
                ret = RPCSVC_AUTH_REJECT;
                goto err;
        }

        if (dict_get (svc->options, srchstr)) {
                ret = dict_get_str (svc->options, srchstr, &valstr);
                if (ret == 0) {
                        ret = gf_string2boolean (srchstr, &insecure);
                        if (ret == 0) {
                                if (insecure == _gf_true)
                                       exportinsecure = RPCSVC_AUTH_ACCEPT;
                                else
                                        exportinsecure = RPCSVC_AUTH_REJECT;
                        } else
                                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to"
                                        " read rpc-auth.ports.insecure value");
                } else
                        gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to"
                                " read rpc-auth.ports.insecure value");
        }

        ret = rpcsvc_combine_gen_spec_volume_checks (globalinsecure,
                                                     exportinsecure);
        if (ret == RPCSVC_AUTH_ACCEPT)
                gf_log (GF_RPCSVC, GF_LOG_DEBUG, "Unprivileged port allowed");
        else
                gf_log (GF_RPCSVC, GF_LOG_DEBUG, "Unprivileged port not"
                        " allowed");

err:
        return ret;
}

/* Inits a rpcsvc_conn_t after accepting the connection.
 */
rpcsvc_conn_t *
rpcsvc_conn_accept_init (rpcsvc_t *svc, int listenfd,
                         rpcsvc_program_t *destprog)
{
        rpcsvc_conn_t   *newconn = NULL;
        int             sock = -1;
        int             ret = -1;

        sock = rpcsvc_socket_accept (listenfd);
        if (sock == -1)
                goto err;

        newconn = rpcsvc_conn_init (svc, destprog, sock);
        if (!newconn) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to init conn object");
                ret = -1;
                goto err;
        }

        rpcsvc_record_init (&newconn->rstate, svc->ctx->iobuf_pool);
        rpcsvc_conn_state_init (newconn);
        if (destprog->conn_init)
                destprog->conn_init (destprog->private, newconn);
        ret = 0;

err:
        if (ret == -1)
                close (sock);

        return newconn;
}


/* Once the connection has been created, we need to associate it with
 * a stage so that the selected stage will handle the event on this connection.
 * This function also allows the caller to decide which handler should
 * be executed in the context of the stage, and also which specific events
 * should be handed to the handler when running in this particular stage.
 */
int
rpcsvc_stage_conn_associate (rpcsvc_stage_t *stg, rpcsvc_conn_t *conn,
                             event_handler_t handler, void *data)
{
        int     ret = -1;

        if ((!stg) || (!conn))
                return -1;

        conn->stage = stg;
        conn->eventidx = event_register (stg->eventpool, conn->sockfd, handler,
                                         data, 1, 0);
        if (conn->eventidx == -1)
                goto err;

        ret = 0;
err:
        return ret;
}


/* Depending on the state we're in, return the size of the next read request. */
size_t
rpcsvc_record_read_size (rpcsvc_record_state_t *rs)
{
        size_t  toread = -1;

        if (!rs)
                return -1;

        if (rpcsvc_record_readfraghdr (rs))
                toread = rs->remainingfraghdr;
        else if (rpcsvc_record_readfrag (rs))
                toread = rs->remainingfrag;
        else
                toread = RPCSVC_CONN_READ;

        return toread;
}


uint32_t
rpcsvc_record_extract_fraghdr (char *fraghdr)
{
        uint32_t        hdr = 0;
        if (!fraghdr)
                return 0;

        memcpy ((void *)&hdr, fraghdr, sizeof (hdr));

        hdr = ntohl (hdr);
        return hdr;
}


ssize_t
rpcsvc_record_read_complete_fraghdr (rpcsvc_record_state_t *rs,ssize_t dataread)
{
        uint32_t        remhdr = 0;
        char            *fraghdrstart = NULL;
        uint32_t        fraghdr = 0;

        fraghdrstart = &rs->fragheader[0];
        remhdr = rs->remainingfraghdr;
        fraghdr = rpcsvc_record_extract_fraghdr (fraghdrstart);
        rs->fragsize = RPCSVC_FRAGSIZE (fraghdr);
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Received fragment size: %d",
                rs->fragsize);
        if (rpcsvc_record_vectored (rs)) {
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC header,"
                        " remaining: %d", RPCSVC_BARERPC_MSGSZ);
                rs->remainingfrag = RPCSVC_BARERPC_MSGSZ;
        } else {
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Regular RPC header,"
                        " remaining: %d", rs->fragsize);
                rs->remainingfrag = rs->fragsize;
        }

        rs->state = RPCSVC_READ_FRAG;
        dataread -= remhdr;
        rs->remainingfraghdr -= remhdr;
        rs->islastfrag = RPCSVC_LASTFRAG (fraghdr);

        return dataread;
}


ssize_t
rpcsvc_record_read_partial_fraghdr (rpcsvc_record_state_t *rs, ssize_t dataread)
{

        /* In case we got less than even the remaining header size,
         * we need to consume it all and wait for remaining frag hdr
         * bytes to come in.
         */
         rs->remainingfraghdr -= dataread;
         rpcsvc_record_update_currenthdr (rs, dataread);
         dataread = 0;
         gf_log (GF_RPCSVC, GF_LOG_TRACE, "Fragment header remaining: %d",
                 rs->remainingfraghdr);

         return dataread;
}


ssize_t
rpcsvc_record_update_fraghdr (rpcsvc_record_state_t *rs, ssize_t dataread)
{
        if ((!rs) || (dataread <= 0))
                return -1;

        /* Why are we even here, we're not supposed to be in the fragment
         * header processing state.
         */
        if (!rpcsvc_record_readfraghdr(rs)) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "record state inconsistent"
                        ": request to update frag header when state is not"
                        "RPCSVC_READ_FRAGHDR");
                return -1;
        }

        /* Again, if header has been read then the state member above should've
         * been different, this is crazy. We should not be here.
         */
        if (rs->remainingfraghdr == 0) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "record state inconsistent"
                        ": request to update frag header when frag header"
                        "remaining is 0.");
                return -1;
        }

        /* We've definitely got the full header now and may be even more. */
        if (dataread >= rs->remainingfraghdr)
                dataread = rpcsvc_record_read_complete_fraghdr (rs, dataread);
        else
                dataread = rpcsvc_record_read_partial_fraghdr (rs, dataread);

        return dataread;
}

ssize_t
rpcsvc_record_read_complete_frag (rpcsvc_record_state_t *rs, ssize_t dataread)
{
        uint32_t        remfrag;

        /* Since the frag is now complete, change the state to the next
         * one, i.e. to read the header of the next fragment.
         */
        remfrag = rs->remainingfrag;
        rs->state = RPCSVC_READ_FRAGHDR;
        dataread -= remfrag;

        /* This will be 0 now. */
        rs->remainingfrag -= remfrag;

        /* Now that the fragment is complete, we must update the
         * record size. Recall that fragsize was got from the frag
         * header.
         */
        rs->recordsize += rs->fragsize;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Fragment remaining: %d",
                rs->remainingfrag);

        return dataread;
}


ssize_t
rpcsvc_record_read_partial_frag (rpcsvc_record_state_t *rs, ssize_t dataread)
{
        /* Just take whatever has come through the current network buffer. */
        rs->remainingfrag -= dataread;

        rpcsvc_record_update_currentfrag (rs, dataread);
        /* Since we know we're consuming the whole buffer from dataread
         * simply setting to 0 zero is fine.
         */
        dataread = 0;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Fragment remaining: %d",
                rs->remainingfrag);
        return dataread;
}


ssize_t
rpcsvc_record_update_frag (rpcsvc_record_state_t *rs, ssize_t dataread)
{
        if ((!rs) || (dataread <= 0))
                return -1;

        if (!rpcsvc_record_readfrag (rs)) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "record state inconsistent"
                        ": request to update fragment when record state is not"
                        "RPCSVC_READ_FRAG.");
                return -1;
        }

        if (rs->remainingfrag == 0) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "record state inconsistent"
                        ": request to update fragment when there is no fragment"
                        " data remaining to be read.");
                return -1;
        }

        /* We've read in more data than the current fragment requires. */
        if (dataread >= rs->remainingfrag)
                dataread = rpcsvc_record_read_complete_frag (rs, dataread);
        else
                dataread = rpcsvc_record_read_partial_frag (rs, dataread);

        return dataread;
}


/* This needs to change to returning errors, since
 * we need to return RPC specific error messages when some
 * of the pointers below are NULL.
 */
rpcsvc_actor_t *
rpcsvc_program_actor (rpcsvc_conn_t *conn, rpcsvc_request_t *req)
{
        rpcsvc_program_t        *program = NULL;
        int                     err = SYSTEM_ERR;
        rpcsvc_actor_t          *actor = NULL;

        if ((!conn) || (!req))
                goto err;

        program = (rpcsvc_program_t *)conn->program;
        if (!program)
                goto err;

        if (req->prognum != program->prognum) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "RPC program not available");
                err = PROG_UNAVAIL;
                goto err;
        }

        if (!program->actors) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "RPC System error");
                err = SYSTEM_ERR;
                goto err;
        }

        if (req->progver != program->progver) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "RPC program version not"
                        " available");
                err = PROG_MISMATCH;
                goto err;
        }

        if ((req->procnum < 0) || (req->procnum >= program->numactors)) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "RPC Program procedure not"
                        " available");
                err = PROC_UNAVAIL;
                goto err;
        }

        actor = &program->actors[req->procnum];
        if (!actor->actor) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "RPC Program procedure not"
                        " available");
                err = PROC_UNAVAIL;
                actor = NULL;
                goto err;
        }

        err = SUCCESS;
        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "Actor found: %s - %s",
                program->progname, actor->procname);
err:
        if (req)
                req->rpc_err = err;

        return actor;
}


rpcsvc_txbuf_t *
rpcsvc_init_txbuf (rpcsvc_conn_t *conn, struct iovec msg, struct iobuf *iob,
                   struct iobref *iobref, int txflags)
{
        rpcsvc_txbuf_t  *txbuf = NULL;

        txbuf = (rpcsvc_txbuf_t *) mem_get(conn->txpool);
        if (!txbuf) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to get txbuf");
                return NULL;
        }

        memset (txbuf, 0, sizeof (*txbuf));
        INIT_LIST_HEAD (&txbuf->txlist);
        txbuf->buf = msg;

        /* If it was required, this iob must've been ref'd already
         *  so I dont have to bother here.
         */
        txbuf->iob = iob;
        txbuf->iobref = iobref;
        txbuf->offset = 0;
        txbuf->txbehave = txflags;

        return txbuf;
}


int
rpcsvc_conn_append_txlist (rpcsvc_conn_t *conn, struct iovec msg,
                           struct iobuf *iob, int txflags)
{
        rpcsvc_txbuf_t          *txbuf = NULL;

        if ((!conn) || (!msg.iov_base) || (!iob))
                return -1;

        txbuf = rpcsvc_init_txbuf (conn, msg, iob, NULL, txflags);
        if (!txbuf)
                return -1;

        list_add_tail (&txbuf->txlist, &conn->txbufs);
        return 0;
}


void
rpcsvc_set_lastfrag (uint32_t *fragsize) {
        (*fragsize) |= 0x80000000U;
}

void
rpcsvc_set_frag_header_size (uint32_t size, char *haddr)
{
        size = htonl (size);
        memcpy (haddr, &size, sizeof (size));
}

void
rpcsvc_set_last_frag_header_size (uint32_t size, char *haddr)
{
        rpcsvc_set_lastfrag (&size);
        rpcsvc_set_frag_header_size (size, haddr);
}


/* Given the RPC reply structure and the payload handed by the RPC program,
 * encode the RPC record header into the buffer pointed by recordstart.
 */
struct iovec
rpcsvc_record_build_header (char *recordstart, size_t rlen,
                            struct rpc_msg reply, size_t payload)
{
        struct iovec    replyhdr;
        struct iovec    txrecord = {0, 0};
        size_t          fraglen = 0;
        int             ret = -1;

        /* After leaving aside the 4 bytes for the fragment header, lets
         * encode the RPC reply structure into the buffer given to us.
         */
        ret = rpc_reply_to_xdr (&reply,(recordstart + RPCSVC_FRAGHDR_SIZE),
                                rlen, &replyhdr);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to create RPC reply");
                goto err;
        }

        fraglen = payload + replyhdr.iov_len;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Reply fraglen %zu, payload: %zu, "
                "rpc hdr: %zu", fraglen, payload, replyhdr.iov_len);

        /* Since we're not spreading RPC records over mutiple fragments
         * we just set this fragment as the first and last fragment for this
         * record.
         */
        rpcsvc_set_last_frag_header_size (fraglen, recordstart);

        /* Even though the RPC record starts at recordstart+RPCSVC_FRAGHDR_SIZE
         * we need to transmit the record with the fragment header, which starts
         * at recordstart.
         */
        txrecord.iov_base = recordstart;

        /* Remember, this is only the vec for the RPC header and does not
         * include the payload above. We needed the payload only to calculate
         * the size of the full fragment. This size is sent in the fragment
         * header.
         */
        txrecord.iov_len = RPCSVC_FRAGHDR_SIZE + replyhdr.iov_len;

err:
        return txrecord;
}


int
rpcsvc_conn_submit (rpcsvc_conn_t *conn, struct iovec hdr,
                    struct iobuf *hdriob, struct iovec msgvec,
                    struct iobuf *msgiob)
{
        int     ret = -1;

        if ((!conn) || (!hdr.iov_base) || (!hdriob))
                return -1;

        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Tx Header: %zu, payload: %zu",
                hdr.iov_len, msgvec.iov_len);
        /* Now that we have both the RPC and Program buffers in xdr format
         * lets hand it to the transmission layer.
         */
        pthread_mutex_lock (&conn->connlock);
        {
                if (!rpcsvc_conn_check_active (conn)) {
                        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "Connection inactive");
                        goto unlock_err;
                }

                ret = rpcsvc_conn_append_txlist (conn, hdr, hdriob,
                                                 RPCSVC_TXB_FIRST);
                if (ret == -1) {
                        gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to append "
                                "header to transmission list");
                        goto unlock_err;
                }

                /* It is possible that this RPC reply is an error reply. In that
                 * case we might not have been handed a payload.
                 */
                ret = 0;
                if (msgiob)
                        ret = rpcsvc_conn_append_txlist (conn, msgvec, msgiob,
                                                         RPCSVC_TXB_LAST);
                if (ret == -1) {
                        gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to append"
                                " payload to transmission list");
                        goto unlock_err;
                }
        }
unlock_err:
        pthread_mutex_unlock (&conn->connlock);

        if (ret == -1)
                goto err;

        /* Tell event pool, we're interested in poll_out to trigger flush
         * of our tx buffers.
         */
        conn->eventidx = event_select_on (conn->stage->eventpool, conn->sockfd,
                                          conn->eventidx, -1, 1);
        ret = 0;
err:

        return ret;
}


int
rpcsvc_fill_reply (rpcsvc_request_t *req, struct rpc_msg *reply)
{
        rpcsvc_program_t        *prog = NULL;
        if ((!req) || (!reply))
                return -1;

        prog = rpcsvc_request_program (req);
        rpc_fill_empty_reply (reply, req->xid);

        if (req->rpc_stat == MSG_DENIED)
                rpc_fill_denied_reply (reply, req->rpc_err, req->auth_err);
        else if (req->rpc_stat == MSG_ACCEPTED)
                rpc_fill_accepted_reply (reply, req->rpc_err, prog->proglowvers,
                                         prog->proghighvers, req->verf.flavour,
                                         req->verf.datalen,
                                         req->verf.authdata);
        else
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Invalid rpc_stat value");

        return 0;
}


/* Given a request and the reply payload, build a reply and encodes the reply
 * into a record header. This record header is encoded into the vector pointed
 * to be recbuf.
 * msgvec is the buffer that points to the payload of the RPC program.
 * This buffer can be NULL, if an RPC error reply is being constructed.
 * The only reason it is needed here is that in case the buffer is provided,
 * we should account for the length of that buffer in the RPC fragment header.
 */
struct iobuf *
rpcsvc_record_build_record (rpcsvc_request_t *req, size_t payload,
                            struct iovec *recbuf)
{
        struct rpc_msg          reply;
        struct iobuf            *replyiob = NULL;
        char                    *record = NULL;
        struct iovec            recordhdr = {0, };
        size_t                  pagesize = 0;
        rpcsvc_conn_t           *conn = NULL;
        rpcsvc_t                *svc = NULL;

        if ((!req) || (!req->conn) || (!recbuf))
                return NULL;

        /* First, try to get a pointer into the buffer which the RPC
         * layer can use.
         */
        conn = req->conn;
        svc = rpcsvc_conn_rpcsvc (conn);
        replyiob = iobuf_get (svc->ctx->iobuf_pool);
        pagesize = iobpool_pagesize ((struct iobuf_pool *)svc->ctx->iobuf_pool);
        if (!replyiob) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to get iobuf");
                goto err_exit;
        }

        record = iobuf_ptr (replyiob);  /* Now we have it. */

        /* Fill the rpc structure and XDR it into the buffer got above. */
        rpcsvc_fill_reply (req, &reply);
        recordhdr = rpcsvc_record_build_header (record, pagesize, reply,
                                                payload);
        if (!recordhdr.iov_base) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to build record "
                        " header");
                iobuf_unref (replyiob);
                replyiob = NULL;
                recbuf->iov_base = NULL;
                goto err_exit;
        }

        recbuf->iov_base = recordhdr.iov_base;
        recbuf->iov_len = recordhdr.iov_len;
err_exit:
        return replyiob;
}


/*
 * The function to submit a program message to the RPC service.
 * This message is added to the transmission queue of the
 * conn.
 *
 * Program callers are not expected to use the msgvec->iov_base
 * address for anything else.
 * Nor are they expected to free it once this function returns.
 * Once the transmission of the buffer is completed by the RPC service,
 * the memory area as referenced through @msg will be unrefed.
 * If a higher layer does not want anything to do with this iobuf
 * after this function returns, it should call unref on it. For keeping
 * it around till the transmission is actually complete, rpcsvc also refs it.
 *  *
 * If this function returns an error by returning -1, the
 * higher layer programs should assume that a disconnection happened
 * and should know that the conn memory area as well as the req structure
 * has been freed internally.
 *
 * For now, this function assumes that a submit is always called
 * to send a new record. Later, if there is a situation where different
 * buffers for the same record come from different sources, then we'll
 * need to change this code to account for multiple submit calls adding
 * the buffers into a single record.
 */

int
rpcsvc_submit_generic (rpcsvc_request_t *req, struct iovec msgvec,
                       struct iobuf *msg)
{
        int                     ret = -1;
        struct iobuf            *replyiob = NULL;
        struct iovec            recordhdr = {0, };
        rpcsvc_conn_t           *conn = NULL;

        if ((!req) || (!req->conn))
                return -1;

        conn = req->conn;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Tx message: %zu", msgvec.iov_len);
        /* Build the buffer containing the encoded RPC reply. */
        replyiob = rpcsvc_record_build_record (req, msgvec.iov_len, &recordhdr);
        if (!replyiob) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR,"Reply record creation failed");
                goto disconnect_exit;
        }

        /* Must ref the iobuf got from higher layer so that the higher layer
         * can rest assured that it can unref it and leave the final freeing
         * of the buffer to us. Note msg can be NULL if an RPC-only message
         * was being sent. Happens when an RPC error reply is being sent.
         */
        if (msg)
                iobuf_ref (msg);
        ret = rpcsvc_conn_submit (conn, recordhdr, replyiob, msgvec, msg);
        mem_put (conn->rxpool, req);

        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to submit message");
                iobuf_unref (replyiob);
        }

disconnect_exit:
        /* Note that a unref is called everytime a reply is sent. This is in
         * response to the ref that is performed on the conn when a request is
         * handed to the RPC program.
         *
         * The catch, however, is that if the reply is an rpc error, we must
         * not unref. This is because the ref only contains
         * references for the actors to which the request was handed plus one
         * reference maintained by the RPC layer. By unrefing for a case where
         * no actor was called, we will be losing the ref held for the RPC
         * layer.
         */
        if ((rpcsvc_request_accepted (req)) &&
            (rpcsvc_request_accepted_success (req)))
                rpcsvc_conn_unref (conn);

        return ret;
}


int
rpcsvc_request_attach_vector (rpcsvc_request_t *req, struct iovec msgvec,
                              struct iobuf *iob, struct iobref *iobref,
                              int finalvector)
{
        rpcsvc_txbuf_t          *txb = NULL;
        int                     txflags = 0;

        if ((!req) || (!msgvec.iov_base))
                return -1;

        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Tx Vector: %zu", msgvec.iov_len);
        if (finalvector)
                txflags |= RPCSVC_TXB_LAST;
        /* We only let the user decide whether this is the last vector for the
         * record, since the first vector is always the RPC header.
         */
        txb = rpcsvc_init_txbuf (req->conn, msgvec, iob, iobref, txflags);
        if (!txb) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Could not init tx buf");
                return -1;
        }

        req->payloadsize += msgvec.iov_len;
        if (iob)
                iobuf_ref (iob);
        if (iobref)
                iobref_ref (iobref);
        list_add_tail (&txb->txlist, &req->txlist);

        return 0;
}


int
rpcsvc_submit_vectors (rpcsvc_request_t *req)
{
        int                     ret = -1;
        struct iobuf            *replyiob = NULL;
        struct iovec            recordhdr = {0, };
        rpcsvc_txbuf_t          *rpctxb = NULL;

        if ((!req) || (!req->conn))
                return -1;

        /* Build the buffer containing the encoded RPC reply. */
        replyiob = rpcsvc_record_build_record (req, req->payloadsize,
                                               &recordhdr);
        if (!replyiob) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR,"Reply record creation failed");
                goto disconnect_exit;
        }

        rpctxb = rpcsvc_init_txbuf (req->conn, recordhdr, replyiob, NULL,
                                    RPCSVC_TXB_FIRST);
        if (!rpctxb) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to create tx buf");
                goto disconnect_exit;
        }

        pthread_mutex_lock (&req->conn->connlock);
        {
                list_splice_init (&req->txlist, &req->conn->txbufs);
                list_add (&rpctxb->txlist, &req->conn->txbufs);
        }
        pthread_mutex_unlock (&req->conn->connlock);

        ret = 0;
        req->conn->eventidx = event_select_on (req->conn->stage->eventpool,
                                               req->conn->sockfd,
                                               req->conn->eventidx, -1, 1);
disconnect_exit:
        /* Note that a unref is called everytime a reply is sent. This is in
         * response to the ref that is performed on the conn when a request is
         * handed to the RPC program.
         */
        rpcsvc_conn_unref (req->conn);
        if (ret == -1)
                iobuf_unref (replyiob);

        mem_put (req->conn->rxpool, req);
        return ret;
}


int
rpcsvc_error_reply (rpcsvc_request_t *req)
{
        struct iovec    dummyvec = {0, };

        if (!req)
                return -1;

        /* At this point the req should already have been filled with the
         * appropriate RPC error numbers.
         */
        return rpcsvc_submit_generic (req, dummyvec, NULL);
}


rpcsvc_request_t *
rpcsvc_request_init (rpcsvc_conn_t *conn, struct rpc_msg *callmsg,
                     struct iovec progmsg, rpcsvc_request_t *req)
{
        if ((!conn) || (!callmsg)|| (!req))
                return NULL;


        /* We start a RPC request as always denied. */
        req->rpc_stat = MSG_DENIED;
        req->xid = rpc_call_xid (callmsg);
        req->prognum = rpc_call_program (callmsg);
        req->progver = rpc_call_progver (callmsg);
        req->procnum = rpc_call_progproc (callmsg);
        req->conn = conn;
        req->msg = progmsg;
        req->recordiob = conn->rstate.activeiob;
        INIT_LIST_HEAD (&req->txlist);
        req->payloadsize = 0;

        /* By this time, the data bytes for the auth scheme would have already
         * been copied into the required sections of the req structure,
         * we just need to fill in the meta-data about it now.
         */
        req->cred.flavour = rpc_call_cred_flavour (callmsg);
        req->cred.datalen = rpc_call_cred_len (callmsg);
        req->verf.flavour = rpc_call_verf_flavour (callmsg);
        req->verf.datalen = rpc_call_verf_len (callmsg);

        /* AUTH */
        rpcsvc_auth_request_init (req);
        return req;
}


rpcsvc_request_t *
rpcsvc_request_create (rpcsvc_conn_t *conn)
{
        char                    *msgbuf = NULL;
        struct rpc_msg          rpcmsg;
        struct iovec            progmsg;        /* RPC Program payload */
        rpcsvc_request_t        *req = NULL;
        int                     ret = -1;

        if (!conn)
                return NULL;

        /* We need to allocate the request before actually calling
         * rpcsvc_request_init on the request so that we, can fill the auth
         * data directly into the request structure from the message iobuf.
         * This avoids a need to keep a temp buffer into which the auth data
         * would've been copied otherwise.
         */
        rpcsvc_alloc_request (conn, req);
        if (!req) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed to alloc request");
                goto err;
        }

        msgbuf = iobuf_ptr (conn->rstate.activeiob);
        ret = xdr_to_rpc_call (msgbuf, conn->rstate.recordsize, &rpcmsg,
                               &progmsg, req->cred.authdata,req->verf.authdata);

        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "RPC call decoding failed");
                rpcsvc_request_seterr (req, GARBAGE_ARGS);
                goto err;
        }

        ret = -1;
        rpcsvc_request_init (conn, &rpcmsg, progmsg, req);

        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "RPC XID: %lx, Ver: %ld, Program: %ld,"
                " ProgVers: %ld, Proc: %ld", rpc_call_xid (&rpcmsg),
                rpc_call_rpcvers (&rpcmsg), rpc_call_program (&rpcmsg),
                rpc_call_progver (&rpcmsg), rpc_call_progproc (&rpcmsg));

        if (rpc_call_rpcvers (&rpcmsg) != 2) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "RPC version not supported");
                rpcsvc_request_seterr (req, RPC_MISMATCH);
                goto err;
        }

        ret = rpcsvc_authenticate (req);
        if (ret == RPCSVC_AUTH_REJECT) {
                /* No need to set auth_err, that is the responsibility of
                 * the authentication handler since only that know what exact
                 * error happened.
                 */
                rpcsvc_request_seterr (req, AUTH_ERROR);
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Failed authentication");
                ret = -1;
                goto err;
        }


        /* If the error is not RPC_MISMATCH, we consider the call as accepted
         * since we are not handling authentication failures for now.
         */
        req->rpc_stat = MSG_ACCEPTED;
        ret = 0;
err:
        if (ret == -1) {
                ret = rpcsvc_error_reply (req);
                req = NULL;
        }

        return req;
}


int
rpcsvc_handle_rpc_call (rpcsvc_conn_t *conn)
{
        rpcsvc_actor_t          *actor = NULL;
        rpcsvc_request_t        *req = NULL;
        int                     ret = -1;

        if (!conn)
                return -1;

        req = rpcsvc_request_create (conn);
        if (!req)
                goto err;

        if (!rpcsvc_request_accepted (req))
                goto err_reply;

        actor = rpcsvc_program_actor (conn, req);
        if (!actor)
                goto err_reply;

        if ((actor) && (actor->actor)) {
                rpcsvc_conn_ref (conn);
                ret = actor->actor (req);
        }

err_reply:
        if (ret == RPCSVC_ACTOR_ERROR)
                ret = rpcsvc_error_reply (req);

        /* No need to propagate error beyond this function since the reply
         * has now been queued. */
        ret = 0;
err:
        return ret;
}

#define rpc_call_cred_addr(rs) (iobuf_ptr ((rs)->activeiob) + RPCSVC_BARERPC_MSGSZ - 4)

uint32_t
rpcsvc_call_credlen (rpcsvc_record_state_t *rs)
{
        char                    *credaddr = NULL;
        uint32_t                credlen_nw = 0;
        uint32_t                credlen_host = 0;

        /* Position to the start of the credential length field. */
        credaddr = rpc_call_cred_addr (rs);
        credlen_nw = *(uint32_t *)credaddr;
        credlen_host = ntohl (credlen_nw);

        return credlen_host;
}

uint32_t
rpcsvc_call_verflen (rpcsvc_record_state_t *rs)
{
        char            *verfaddr = NULL;
        uint32_t        verflen_nw = 0;
        uint32_t        verflen_host = 0;
        uint32_t        credlen = 0;

        /* Position to the start of the verifier length field. */
        credlen = rpcsvc_call_credlen (rs);
        verfaddr = (rpc_call_cred_addr (rs) + 4 + credlen);
        verflen_nw = *(uint32_t *)verfaddr;
        verflen_host = ntohl (verflen_nw);

        return verflen_host;
}


void
rpcsvc_update_vectored_verf (rpcsvc_record_state_t *rs)
{
        if (!rs)
                return;

        rs->recordsize += rpcsvc_call_verflen (rs);
        return;
}


void
rpcsvc_handle_vectored_prep_rpc_call (rpcsvc_conn_t *conn)
{
        rpcsvc_actor_t          *actor = NULL;
        rpcsvc_request_t        *req = NULL;
        rpcsvc_record_state_t   *rs = NULL;
        rpcsvc_t                *svc = NULL;
        int                     ret = -1;
        ssize_t                 remfrag = RPCSVC_ACTOR_ERROR;
        int                     newbuf = 0;

        if (!conn)
                return;

        rs = &conn->rstate;

        /* In case one of the steps below fails, we need to make sure that the
         * remaining frag in the kernel's buffers are read-out so that the
         * requests that follow can be served.
         */
        rs->remainingfrag = rs->fragsize - rs->recordsize;
        rs->vecstate = RPCSVC_VECTOR_IGNORE;
        req = rpcsvc_request_create (conn);
        svc = rpcsvc_conn_rpcsvc (conn);
        if (!req)
                goto err;

        if (!rpcsvc_request_accepted (req))
                goto err_reply;

        actor = rpcsvc_program_actor (conn, req);
        if (!actor)
                goto err_reply;

        if (!actor->vector_sizer) {
                ret = -1;
                rpcsvc_request_seterr (req, PROC_UNAVAIL);
                goto err_reply;
        }

        rpcsvc_conn_ref (conn);
        ret = actor->vector_sizer (req, &remfrag, &newbuf);
        rpcsvc_conn_unref (conn);

        if (ret == RPCSVC_ACTOR_ERROR) {
                ret = -1;
                rpcsvc_request_seterr (req, SYSTEM_ERR);
                goto err_reply;
        }

        rs->remainingfrag = remfrag;
        rs->vecstate = RPCSVC_VECTOR_READPROCHDR;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC proc header remaining:"
                " %d", rs->remainingfrag);
        conn->vectoredreq = req;

        /* Store the reference to the current frag pointer. This is where the
         * proc header will be read into.
         */
        req->msg.iov_base = rs->fragcurrent;
        req->msg.iov_len = rs->remainingfrag;
        ret = 0;

err_reply:
        if (ret == -1)
                ret = rpcsvc_error_reply (req);

        /* No need to propagate error beyond this function since the reply
         * has now been queued. */
        ret = 0;
err:
        return;
}


void
rpcsvc_update_vectored_verfsz (rpcsvc_conn_t *conn)
{
        rpcsvc_record_state_t   *rs = NULL;
        uint32_t                verflen = 0;

        if (!conn)
                return;

        rs = &conn->rstate;

        verflen = rpcsvc_call_verflen (rs);
        rs->recordsize += 8;
        if (verflen > 0) {
                rs->remainingfrag = verflen;
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC verf remaining: "
                        " %d", rs->remainingfrag);
                rs->vecstate = RPCSVC_VECTOR_READVERF;
        } else {
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC preparing call");
                rpcsvc_handle_vectored_prep_rpc_call (conn);
        }

        return;
}


void
rpcsvc_update_vectored_cred (rpcsvc_record_state_t *rs)
{
        uint32_t                credlen = 0;

        if (!rs)
                return;

        credlen = rpcsvc_call_credlen (rs);
        /* Update remainingfrag to read the 8 bytes needed for
         * reading verf flavour and verf len.
         */
        rs->remainingfrag = (2 * sizeof (uint32_t));
        rs->vecstate = RPCSVC_VECTOR_READVERFSZ;
        rs->recordsize += credlen;
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC verfsz remaining: %d",
                rs->remainingfrag);

        return;
}

void
rpcsvc_update_vectored_barerpc (rpcsvc_record_state_t *rs)
{
        uint32_t                credlen = 0;

        if (!rs)
                return;

        credlen = rpcsvc_call_credlen (rs);
        rs->recordsize = RPCSVC_BARERPC_MSGSZ;
        if (credlen == 0) {
                rs->remainingfrag = 8;
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC verfsz remaining"
                        ": %d", rs->remainingfrag);
                rs->vecstate = RPCSVC_VECTOR_READVERFSZ;
        } else {
                rs->remainingfrag = credlen;
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC cred remaining: "
                        "%d", rs->remainingfrag);
                rs->vecstate = RPCSVC_VECTOR_READCRED;
        }

        return;
}


void
rpcsvc_handle_vectored_rpc_call (rpcsvc_conn_t *conn)
{
        rpcsvc_actor_t          *actor = NULL;
        rpcsvc_request_t        *req = NULL;
        rpcsvc_record_state_t   *rs = NULL;
        rpcsvc_t                *svc = NULL;
        int                     ret = -1;
        ssize_t                 remfrag = -1;
        int                     newbuf = 0;

        if (!conn)
                return;

        rs = &conn->rstate;

        req = conn->vectoredreq;
        svc = rpcsvc_conn_rpcsvc (conn);

        if (!req)
                goto err;

        actor = rpcsvc_program_actor (conn, req);
        if (!actor)
                goto err_reply;

        if (!actor->vector_sizer) {
                ret = -1;
                rpcsvc_request_seterr (req, PROC_UNAVAIL);
                goto err_reply;
        }

        req->msg.iov_len = (unsigned long)((long)rs->fragcurrent - (long)req->msg.iov_base);
        rpcsvc_conn_ref (conn);
        ret = actor->vector_sizer (req, &remfrag, &newbuf);
        rpcsvc_conn_unref (conn);
        if (ret == RPCSVC_ACTOR_ERROR) {
                ret = -1;
                rpcsvc_request_seterr (req, SYSTEM_ERR);
                goto err_reply;
        }

        if (newbuf) {
                rs->vectoriob = iobuf_get (svc->ctx->iobuf_pool);
                rs->fragcurrent = iobuf_ptr (rs->vectoriob);
                rs->vecstate = RPCSVC_VECTOR_READVEC;
                rs->remainingfrag = remfrag;
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC buf remaining:"
                        " %d", rs->remainingfrag);
        } else {
                rs->remainingfrag = remfrag;
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC proc remaining:"
                        " %d", rs->remainingfrag);
        }

        ret = 0;
err_reply:
        if (ret == -1)
                ret = rpcsvc_error_reply (req);

        /* No need to propagate error beyond this function since the reply
         * has now been queued. */
        ret = 0;
err:
        return;
}



void
rpcsvc_record_vectored_call_actor (rpcsvc_conn_t *conn)
{
        rpcsvc_actor_t          *actor = NULL;
        rpcsvc_request_t        *req = NULL;
        rpcsvc_record_state_t   *rs = NULL;
        rpcsvc_t                *svc = NULL;
        int                     ret = -1;

        if (!conn)
                return;

        rs = &conn->rstate;
        req = conn->vectoredreq;
        svc = rpcsvc_conn_rpcsvc (conn);

        if (!req)
                goto err;

        actor = rpcsvc_program_actor (conn, req);
        if (!actor)
                goto err_reply;

        if (actor->vector_actor) {
                rpcsvc_conn_ref (conn);
                ret = actor->vector_actor (req, rs->vectoriob);
        } else {
                rpcsvc_request_seterr (req, PROC_UNAVAIL);
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "No vectored handler present");
                ret = RPCSVC_ACTOR_ERROR;
        }

err_reply:
        if (ret == RPCSVC_ACTOR_ERROR)
                ret = rpcsvc_error_reply (req);

        /* No need to propagate error beyond this function since the reply
         * has now been queued. */
        ret = 0;
err:
        return;
}



ssize_t
rpcsvc_update_vectored_state (rpcsvc_conn_t *conn)
{
        rpcsvc_record_state_t   *rs = NULL;
        rpcsvc_t                *svc = NULL;

        if (!conn)
                return 0;

        /* At this point, we can be confident that the activeiob contains
         * exactly the first RPCSVC_BARERPC_MSGSZ bytes needed in order to
         * determine the program and actor. So the next state will be
         * to read the credentials.
         *
         * But first, try to determine how many more bytes do we need from the
         * network to complete the RPC message including the credentials.
         */

        rs = &conn->rstate;
        if (rpcsvc_record_vectored_baremsg (rs))
                rpcsvc_update_vectored_barerpc (rs);
        else if (rpcsvc_record_vectored_cred (rs))
                rpcsvc_update_vectored_cred (rs);
        else if (rpcsvc_record_vectored_verfsz (rs))
                rpcsvc_update_vectored_verfsz (conn);
        else if (rpcsvc_record_vectored_verfread (rs)) {
                rpcsvc_update_vectored_verf (rs);
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC preparing call");
                rpcsvc_handle_vectored_prep_rpc_call (conn);
        } else if (rpcsvc_record_vectored_readprochdr (rs))
                rpcsvc_handle_vectored_rpc_call (conn);
        else if (rpcsvc_record_vectored_ignore (rs)) {
                svc = rpcsvc_conn_rpcsvc (conn);
                rpcsvc_record_init (rs, svc->ctx->iobuf_pool);
        } else if (rpcsvc_record_vectored_readvec (rs)) {
                svc = rpcsvc_conn_rpcsvc (conn);
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored RPC vector read");
                rpcsvc_record_vectored_call_actor (conn);
                rpcsvc_record_init (rs, svc->ctx->iobuf_pool);
        }

        return 0;
}


ssize_t
rpcsvc_record_read_partial_frag (rpcsvc_record_state_t *rs, ssize_t dataread);

ssize_t
rpcsvc_update_vectored_msg (rpcsvc_conn_t *conn, ssize_t dataread)
{

        if (!conn)
                return dataread;

        /* find out how much of the bare msg is pending and set that up to be
         * read into the updated fragcurrent along with the updated size into
         * remainingfrag.
         */


        /* Incidently, the logic needed here is similar to a regular partial
         * fragment read since we've already set the remainingfrag member in
         * rstate to be RPCSVC_BARERPC_MSGSZ for the purpose of a vectored
         * fragment.
         */
        return rpcsvc_record_read_partial_frag (&conn->rstate, dataread);
}

/* FIX: As a first version of vectored reading, I am assuming dataread will
 * always be equal to RPCSVC_BARERPC_MSGSZ for the sake of simplicity on the
 * belief that we're never actually reading more bytes than needed in each
 * poll_in.
 */
ssize_t
rpcsvc_handle_vectored_frag (rpcsvc_conn_t *conn, ssize_t dataread)
{
        if (!conn)
                return dataread;

        /* At this point we can be confident that only the frag size has been
         * read from the network. Now it is up to us to have the remaining RPC
         * fields given to us here.
         */

        /* Since the poll_in handler uses the remainingfrag field to determine
         * how much to read from the network, we'll hack this scheme to tell
         * the poll_in to read at most RPCSVC_BARERPC_MSGSZ bytes. This is done
         * to, as a first step, identify which (program, actor) we need to call.
         */

        dataread = rpcsvc_update_vectored_msg (conn, dataread);

        if (conn->rstate.remainingfrag == 0) {
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored frag complete");
                dataread = rpcsvc_update_vectored_state (conn);
        }

        return dataread;
}


int
rpcsvc_record_update_state (rpcsvc_conn_t *conn, ssize_t dataread)
{
        rpcsvc_record_state_t   *rs = NULL;
        rpcsvc_t                *svc = NULL;

        if (!conn)
                return -1;

        rs = &conn->rstate;
        /* At entry into this function, fragcurrent will be pointing to the\
         * start of the area into which dataread number of bytes were read.
         */

        if (rpcsvc_record_readfraghdr(rs))
                dataread = rpcsvc_record_update_fraghdr (rs, dataread);

        if (rpcsvc_record_readfrag(rs)) {
                /* The minimum needed for triggering the vectored handler is
                 * the frag size field. The fragsize member remains set to this
                 * size till this request is completely extracted from the
                 * network. Once all the data has been read from the network,
                 * the request structure would've been created. The point being
                 * that even if it takes multiple calls to network IO for
                 * getting the vectored fragment, we can continue to use this
                 * condition as the flag to tell us that this is a vectored
                 * fragment.
                 */
                if ((dataread > 0) && (rpcsvc_record_vectored (rs))) {
                        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Vectored frag");
                        dataread = rpcsvc_handle_vectored_frag (conn, dataread);
                } else if (dataread > 0) {
                        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Regular frag");
                        dataread = rpcsvc_record_update_frag (rs, dataread);
                }
        }

        /* This should not happen. We are never reading more than the current
         * fragment needs. Something is seriously wrong.
         */
        if (dataread > 0) {
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Data Left: %zd", dataread);
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Unwanted data read from "
                        " connection.");
        }

        /* If we're now supposed to wait for a new fragment header and if the
         * fragment that we just completed in the previous call to
         * rpcsvc_record_update_frag was the last fragment for the current
         * RPC record, then, it is time to perform the translation from
         * XDR formatted buffer in activeiob followed by the upcall to the
         * protocol actor.
         */
        if ((rpcsvc_record_readfraghdr(rs)) && (rs->islastfrag)) {
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "Full Record Received.");
                rpcsvc_handle_rpc_call (conn);
                svc = rpcsvc_conn_rpcsvc (conn);
                rpcsvc_record_init (rs, svc->ctx->iobuf_pool);
        }

        return 0;
}


char *
rpcsvc_record_read_addr (rpcsvc_record_state_t *rs)
{

        if (rpcsvc_record_readfraghdr (rs))
                return rpcsvc_record_currenthdr_addr (rs);
        else if (rpcsvc_record_readfrag (rs))
                return rpcsvc_record_currentfrag_addr (rs);

        return NULL;
}


int
rpcsvc_conn_data_poll_in (rpcsvc_conn_t *conn)
{
        ssize_t         dataread = -1;
        size_t          readsize = 0;
        char            *readaddr = NULL;
        int             ret = -1;

        readaddr = rpcsvc_record_read_addr (&conn->rstate);
        if (!readaddr)
                goto err;

        readsize = rpcsvc_record_read_size (&conn->rstate);
        if (readsize == -1)
                goto err;

        dataread = rpcsvc_socket_read (conn->sockfd, readaddr, readsize);
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "conn: 0x%lx, readsize: %zu, dataread: %zd",
                (long)conn, readsize, dataread);

        if (dataread > 0)
                ret = rpcsvc_record_update_state (conn, dataread);

err:
        return ret;
}


int
rpcsvc_conn_data_poll_err (rpcsvc_conn_t *conn)
{
        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Received error event");
        rpcsvc_conn_deinit (conn);
        return 0;
}


int
__rpcsvc_conn_data_poll_out (rpcsvc_conn_t *conn)
{
        rpcsvc_txbuf_t          *txbuf = NULL;
        rpcsvc_txbuf_t          *tmp = NULL;
        ssize_t                 written = -1;
        char                    *writeaddr = NULL;
        size_t                  writesize = -1;

        if (!conn)
                return -1;

        /* Attempt transmission of each of the pending buffers */
        list_for_each_entry_safe (txbuf, tmp, &conn->txbufs, txlist) {
tx_remaining:
                writeaddr = (char *)(txbuf->buf.iov_base + txbuf->offset);
                writesize = (txbuf->buf.iov_len - txbuf->offset);

                if (txbuf->txbehave & RPCSVC_TXB_FIRST) {
                        gf_log (GF_RPCSVC, GF_LOG_TRACE, "First Tx Buf");
                        rpcsvc_socket_block_tx (conn->sockfd);
                }

                written = rpcsvc_socket_write (conn->sockfd, writeaddr,
                                               writesize);
                if (txbuf->txbehave & RPCSVC_TXB_LAST) {
                        gf_log (GF_RPCSVC, GF_LOG_TRACE, "Last Tx Buf");
                        rpcsvc_socket_unblock_tx (conn->sockfd);
                }
                gf_log (GF_RPCSVC, GF_LOG_TRACE, "conn: 0x%lx, Tx request: %zu,"
                        " Tx sent: %zd", (long)conn, writesize, written);

                /* There was an error transmitting this buffer */
                if (written == -1)
                        break;

                if (written >= 0)
                        txbuf->offset += written;

                /* If the current buffer has been completely transmitted,
                 * delete it from the list and move on to the next buffer.
                 */
                if (txbuf->offset == txbuf->buf.iov_len) {
                        /* It doesnt matter who ref'ed this iobuf, rpcsvc for
                         * its own header or a RPC program.
                         */
                        if (txbuf->iob)
                                iobuf_unref (txbuf->iob);
                        if (txbuf->iobref)
                                iobref_unref (txbuf->iobref);

                        list_del (&txbuf->txlist);
                        mem_put (conn->txpool, txbuf);
                } else
                        /* If the current buffer is incompletely tx'd, do not
                         * go to the head of the loop, since that moves us to
                         * the next buffer.
                         */
                        goto tx_remaining;
        }

        /* If we've broken out of the loop above then we must unblock
         * the transmission now.
         */
        rpcsvc_socket_unblock_tx (conn->sockfd);
        if (list_empty (&conn->txbufs))
                conn->eventidx = event_select_on (conn->stage->eventpool,
                                                  conn->sockfd, conn->eventidx,
                                                  -1, 0);

        return 0;
}


int
rpcsvc_conn_data_poll_out (rpcsvc_conn_t *conn)
{
        if (!conn)
                return -1;


        pthread_mutex_lock (&conn->connlock);
        {
                __rpcsvc_conn_data_poll_out (conn);
        }
        pthread_mutex_unlock (&conn->connlock);

        return 0;
}


int
rpcsvc_conn_data_handler (int fd, int idx, void *data, int poll_in, int poll_out
                          , int poll_err)
{
        rpcsvc_conn_t   *conn = NULL;
        int             ret = 0;

        if (!data)
                return 0;

        conn = (rpcsvc_conn_t *)data;

        if (poll_out)
                ret = rpcsvc_conn_data_poll_out (conn);

        if (poll_err) {
                ret = rpcsvc_conn_data_poll_err (conn);
                return 0;
        }

        if (poll_in) {
                ret = 0;
                ret = rpcsvc_conn_data_poll_in (conn);
        }

        if (ret == -1)
                rpcsvc_conn_data_poll_err (conn);

        return 0;
}


int
rpcsvc_conn_listening_handler (int fd, int idx, void *data, int poll_in,
                               int poll_out, int poll_err)
{
        rpcsvc_conn_t           *newconn = NULL;
        rpcsvc_stage_t          *selectedstage = NULL;
        int                     ret = -1;
        rpcsvc_conn_t           *conn = NULL;
        rpcsvc_program_t        *prog = NULL;
        rpcsvc_t                *svc = NULL;

        if (!poll_in)
                return 0;

        conn = (rpcsvc_conn_t *)data;
        prog = (rpcsvc_program_t *)conn->program;
        svc = rpcsvc_conn_rpcsvc (conn);
        newconn = rpcsvc_conn_accept_init (svc, fd, prog);
        if (!newconn) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "failed to accept connection");
                goto err;
        }

        selectedstage = rpcsvc_select_stage (svc);
        if (!selectedstage)
                goto close_err;

        /* Now that we've accepted the connection, we need to associate
         * its events to a stage.
         */
        ret = rpcsvc_stage_conn_associate (selectedstage, newconn,
                                           rpcsvc_conn_data_handler, newconn);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "could not associated stage "
                        " with new connection");
                goto close_err;
        }
        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "New Connection: Program %s, Num: %d,"
                " Ver: %d, Port: %d", prog->progname, prog->prognum,
                prog->progver, prog->progport);
        ret = 0;
close_err:
        if (ret == -1)
                rpcsvc_conn_unref (newconn);

err:
        return ret;
}


/* Register the program with the local portmapper service. */
int
rpcsvc_program_register_portmap (rpcsvc_program_t *newprog)
{
        if (!newprog)
                return -1;

        if (!(pmap_set(newprog->prognum, newprog->progver, IPPROTO_TCP,
                       newprog->progport))) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Could not register with"
                        " portmap");
                return -1;
        }

        return 0;
}


int
rpcsvc_program_unregister_portmap (rpcsvc_program_t *prog)
{
        if (!prog)
                return -1;

        if (!(pmap_unset(prog->prognum, prog->progver))) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Could not unregister with"
                        " portmap");
                return -1;
        }

        return 0;
}


int
rpcsvc_stage_program_register (rpcsvc_stage_t *stg, rpcsvc_program_t *newprog)
{
        rpcsvc_conn_t           *newconn = NULL;
        rpcsvc_t                *svc = NULL;

        if ((!stg) || (!newprog))
                return -1;

        svc = rpcsvc_stage_service (stg);
        /* Create a listening socket */
        newconn = rpcsvc_conn_listen_init (svc, newprog);
        if (!newconn) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "could not create listening"
                        " connection");
                return -1;
        }

        if ((rpcsvc_stage_conn_associate (stg, newconn,
                                          rpcsvc_conn_listening_handler,
                                          newconn)) == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR,"could not associate stage with"
                        " listening connection");
                return -1;
        }

        return 0;
}


int
rpcsvc_program_register (rpcsvc_t *svc, rpcsvc_program_t program)
{
        rpcsvc_program_t        *newprog = NULL;
        rpcsvc_stage_t          *selectedstage = NULL;
        int                     ret = -1;

        if (!svc)
                return -1;

        newprog = GF_CALLOC (1, sizeof(*newprog), gf_common_mt_rpcsvc_program_t);
        if (!newprog)
                return -1;

        if (!program.actors)
                goto free_prog;

        memcpy (newprog, &program, sizeof (program));
        selectedstage = rpcsvc_select_stage (svc);

        ret = rpcsvc_stage_program_register (selectedstage, newprog);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "stage registration of program"
                        " failed");
                goto free_prog;
        }

        ret = rpcsvc_program_register_portmap (newprog);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "portmap registration of"
                        " program failed");
                goto free_prog;
        }

        ret = 0;
        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "New program registered: %s, Num: %d,"
                " Ver: %d, Port: %d", newprog->progname, newprog->prognum,
                newprog->progver, newprog->progport);

free_prog:
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Program registration failed:"
                        " %s, Num: %d, Ver: %d, Port: %d", newprog->progname,
                        newprog->prognum, newprog->progver, newprog->progport);
                GF_FREE (newprog);
        }

        return ret;
}

/* The only difference between the generic submit and this one is that the
 * generic submit is also used for submitting RPC error replies in where there
 * are no payloads so the msgvec and msgbuf can be NULL.
 * Since RPC programs should be using this function along with their payloads
 * we must perform NULL checks before calling the generic submit.
 */
int
rpcsvc_submit_message (rpcsvc_request_t *req, struct iovec msgvec,
                       struct iobuf *msg)
{
        if ((!req) || (!req->conn) || (!msg) || (!msgvec.iov_base))
                return -1;

        return rpcsvc_submit_generic (req, msgvec, msg);
}


int
rpcsvc_program_unregister (rpcsvc_t *svc, rpcsvc_program_t prog)
{
        int                     ret = -1;

        if (!svc)
                return -1;

        /* TODO: De-init the listening connection for this program. */
        ret = rpcsvc_program_unregister_portmap (&prog);
        if (ret == -1) {
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "portmap unregistration of"
                        " program failed");
                goto err;
        }

        ret = 0;
        gf_log (GF_RPCSVC, GF_LOG_DEBUG, "Program unregistered: %s, Num: %d,"
                " Ver: %d, Port: %d", prog.progname, prog.prognum,
                prog.progver, prog.progport);

err:
        if (ret == -1)
                gf_log (GF_RPCSVC, GF_LOG_ERROR, "Program unregistration failed"
                        ": %s, Num: %d, Ver: %d, Port: %d", prog.progname,
                        prog.prognum, prog.progver, prog.progport);

        return ret;
}


int
rpcsvc_conn_peername (rpcsvc_conn_t *conn, char *hostname, int hostlen)
{
        if (!conn)
                return -1;

        return rpcsvc_socket_peername (conn->sockfd, hostname, hostlen);
}


int
rpcsvc_conn_peeraddr (rpcsvc_conn_t *conn, char *addrstr, int addrlen,
                      struct sockaddr *sa, socklen_t sasize)
{
        if (!conn)
                return -1;

        return rpcsvc_socket_peeraddr (conn->sockfd, addrstr, addrlen, sa,
                                       sasize);
}
