/* Copyright (c) 2015, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "main.h"
#include "stat_internal.h"
#include "hiredis.h"
#include "upstream.h"

#define REDIS_CTX(p) (struct redis_stat_ctx *)(p)
#define REDIS_RUNTIME(p) (struct redis_stat_runtime *)(p)
#define REDIS_BACKEND_TYPE "redis"
#define REDIS_DEFAULT_PORT 6379
#define REDIS_DEFAULT_OBJECT "%s%l"

struct redis_stat_ctx {
	struct upstream_list *read_servers;
	struct upstream_list *write_servers;

	const gchar *redis_object;
	gdouble timeout;
};

struct redis_stat_runtime {
	struct rspamd_task *task;
	struct upstream *selected;
	GArray *results;
	gchar *redis_object_expanded;
};

#define GET_TASK_ELT(task, elt) (task == NULL ? NULL : (task)->elt)

/*
 * Non-static for lua unit testing
 */
gsize
rspamd_redis_expand_object (const gchar *pattern,
		struct rspamd_statfile_config *stcf,
		struct rspamd_task *task,
		gchar **target)
{
	gsize tlen = 0;
	const gchar *p = pattern, *elt;
	InternetAddressList *ia;
	InternetAddress *iaelt;
	InternetAddressMailbox *imb;
	gchar *d, *end;
	enum  {
		just_char,
		percent_char,
		mod_char
	} state = just_char;

	g_assert (stcf != NULL);

	/* Length calculation */
	while (*p) {
		switch (state) {
		case just_char:
			if (*p == '%') {
				state = percent_char;
			}
			else {
				tlen ++;
			}
			p ++;
			break;
		case percent_char:
			switch (*p) {
			case '%':
				tlen ++;
				state = just_char;
				break;
			case 'f':
				if (task) {
					elt = rspamd_task_get_sender (task);
					if (elt) {
						tlen += strlen (elt);
					}
				}
				break;
			case 'u':
				elt = GET_TASK_ELT (task, user);
				if (elt) {
					tlen += strlen (elt);
				}
				break;
			case 'r':
				ia = GET_TASK_ELT (task, rcpt_envelope);
				if (ia != NULL) {
					iaelt = internet_address_list_get_address (ia, 0);
					imb = INTERNET_ADDRESS_IS_MAILBOX (iaelt) ?
								INTERNET_ADDRESS_MAILBOX (iaelt) : NULL;

					elt = (imb ? internet_address_mailbox_get_addr (imb) : NULL);

					if (elt) {
						tlen += strlen (elt);
					}
				}
				break;
			case 'l':
				if (stcf->label) {
					tlen += strlen (stcf->label);
				}
				break;
			case 's':
				if (stcf->symbol) {
					tlen += strlen (stcf->symbol);
				}
				break;
			default:
				state = just_char;
				tlen ++;
				break;
			}

			if (state == percent_char) {
				state = mod_char;
			}
			p ++;
			break;

		case mod_char:
			switch (*p) {
			case 'd':
				p ++;
				state = just_char;
				break;
			default:
				state = just_char;
				break;
			}
			break;
		}
	}

	if (target == NULL) {
		return tlen;
	}

	*target = rspamd_mempool_alloc (task->task_pool, tlen + 1);
	d = *target;
	end = d + tlen + 1;
	d[tlen] = '\0';
	p = pattern;
	state = just_char;

	/* Expand string */
	while (*p && d < end) {
		switch (state) {
		case just_char:
			if (*p == '%') {
				state = percent_char;
			}
			else {
				*d++ = *p;
			}
			p ++;
			break;
		case percent_char:
			switch (*p) {
			case '%':
				*d++ = *p;
				state = just_char;
				break;
			case 'f':
				if (task) {
					elt = rspamd_task_get_sender (task);
					if (elt) {
						d += rspamd_strlcpy (d, elt, end - d);
					}
				}
				break;
			case 'u':
				elt = GET_TASK_ELT (task, user);
				if (elt) {
					d += rspamd_strlcpy (d, elt, end - d);
				}
				break;
			case 'r':
				ia = GET_TASK_ELT (task, rcpt_envelope);
				if (ia != NULL) {
					iaelt = internet_address_list_get_address (ia, 0);
					imb = INTERNET_ADDRESS_IS_MAILBOX (iaelt) ?
							INTERNET_ADDRESS_MAILBOX (iaelt) : NULL;

					elt = (imb ? internet_address_mailbox_get_addr (imb) : NULL);

					if (elt) {
						d += rspamd_strlcpy (d, elt, end - d);
					}
				}
				break;
			case 'l':
				if (stcf->label) {
					d += rspamd_strlcpy (d, stcf->label, end - d);
				}
				break;
			case 's':
				if (stcf->symbol) {
					d += rspamd_strlcpy (d, stcf->symbol, end - d);
				}
				break;
			default:
				state = just_char;
				*d++ = *p;
				break;
			}

			if (state == percent_char) {
				state = mod_char;
			}
			p ++;
			break;

		case mod_char:
			switch (*p) {
			case 'd':
				/* TODO: not supported yet */
				p ++;
				state = just_char;
				break;
			default:
				state = just_char;
				break;
			}
			break;
		}
	}

	return tlen;
}

gpointer
rspamd_redis_init (struct rspamd_stat_ctx *ctx, struct rspamd_config *cfg)
{
	struct redis_stat_ctx *new;
	struct rspamd_classifier_config *clf;
	struct rspamd_statfile_config *stf;
	GList *cur, *curst;
	const ucl_object_t *elt;

	new = rspamd_mempool_alloc0 (cfg->cfg_pool, sizeof (*new));

	/* Iterate over all classifiers and load matching statfiles */
	cur = cfg->classifiers;

	while (cur) {
		clf = cur->data;

		curst = clf->statfiles;
		while (curst) {
			stf = curst->data;

			/*
			 * By default, all statfiles are treated as mmaped files
			 */
			if (stf->backend != NULL && strcmp (stf->backend, REDIS_BACKEND_TYPE)) {
				/*
				 * Check configuration sanity
				 */
				elt = ucl_object_find_key (stf->opts, "read_servers");
				if (elt == NULL) {
					elt = ucl_object_find_key (stf->opts, "servers");
				}
				if (elt == NULL) {
					msg_err ("statfile %s has no redis servers", stf->symbol);
					curst = curst->next;
					continue;
				}
				else {
					new->read_servers = rspamd_upstreams_create ();
					if (!rspamd_upstreams_from_ucl (new->read_servers, elt,
							REDIS_DEFAULT_PORT, NULL)) {
						msg_err ("statfile %s cannot read servers configuration",
								stf->symbol);
						curst = curst->next;
						continue;
					}
				}

				elt = ucl_object_find_key (stf->opts, "write_servers");
				if (elt == NULL) {
					msg_err ("statfile %s has no write redis servers, "
							"so learning is impossible", stf->symbol);
					curst = curst->next;
					continue;
				}
				else {
					new->write_servers = rspamd_upstreams_create ();
					if (!rspamd_upstreams_from_ucl (new->read_servers, elt,
							REDIS_DEFAULT_PORT, NULL)) {
						msg_err ("statfile %s cannot write servers configuration",
								stf->symbol);
						rspamd_upstreams_destroy (new->write_servers);
						new->write_servers = NULL;
					}
				}

				elt = ucl_object_find_key (stf->opts, "prefix");
				if (elt == NULL || ucl_object_type (elt) != UCL_STRING) {
					new->redis_object = REDIS_DEFAULT_OBJECT;
				}
				else {
					/* XXX: sanity check */
					new->redis_object = ucl_object_tostring (elt);
					if (rspamd_redis_expand_object (new->redis_object, stf,
							NULL, NULL) == 0) {
						msg_err ("statfile %s cannot write servers configuration",
							stf->symbol);
					}
				}

				ctx->statfiles ++;
			}

			curst = curst->next;
		}

		cur = g_list_next (cur);
	}

	return (gpointer)new;
}

gpointer
rspamd_redis_runtime (struct rspamd_task *task,
		struct rspamd_statfile_config *stcf,
		gboolean learn, gpointer ctx)
{

}

gboolean rspamd_redis_process_token (struct token_node_s *tok,
		struct rspamd_token_result *res,
		gpointer ctx);
gboolean rspamd_redis_learn_token (struct token_node_s *tok,
		struct rspamd_token_result *res,
		gpointer ctx);
void rspamd_redis_finalize_learn (struct rspamd_statfile_runtime *runtime,
		gpointer ctx);
gulong rspamd_redis_total_learns (struct rspamd_statfile_runtime *runtime,
		gpointer ctx);
gulong rspamd_redis_inc_learns (struct rspamd_statfile_runtime *runtime,
		gpointer ctx);
gulong rspamd_redis_learns (struct rspamd_statfile_runtime *runtime,
		gpointer ctx);
ucl_object_t * rspamd_redis_get_stat (struct rspamd_statfile_runtime *runtime,
		gpointer ctx);
