/*
* Copyright (c) 2018,2019 Travelping GmbH
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at:
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <assert.h>
#include <vnet/vnet.h>
#include <vnet/session/application.h>
#include <vnet/session/application_interface.h>
#include <vnet/session/session.h>

#include "upf.h"
#include "upf_pfcp.h"
#include "upf_pfcp_api.h"
#include "upf_proxy.h"

typedef enum
{
  EVENT_WAKEUP = 1,
} http_process_event_t;

upf_proxy_main_t upf_proxy_main;

static void
proxy_server_sessions_reader_lock (void)
{
  clib_rwlock_reader_lock (&upf_proxy_main.sessions_lock);
}

static void
proxy_server_sessions_reader_unlock (void)
{
  clib_rwlock_reader_unlock (&upf_proxy_main.sessions_lock);
}

static void
proxy_server_sessions_writer_lock (void)
{
  clib_rwlock_writer_lock (&upf_proxy_main.sessions_lock);
}

static void
proxy_server_sessions_writer_unlock (void)
{
  clib_rwlock_writer_unlock (&upf_proxy_main.sessions_lock);
}

static upf_proxy_session_t *
proxy_session_alloc (u32 thread_index)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  upf_proxy_session_t *ps;

  pool_get (pm->sessions, ps);
  clib_memset (ps, 0, sizeof (*ps));
  ps->session_index = ps - pm->sessions;

  ps->proxy_session_index = ~0;
  ps->active_open_session_index = ~0;

  return ps;
}

static upf_proxy_session_t *
proxy_session_get (u32 ps_index)
{
  upf_proxy_main_t *pm = &upf_proxy_main;

  if (pool_is_free_index (pm->sessions, ps_index))
    return 0;
  return pool_elt_at_index (pm->sessions, ps_index);
}

static void
proxy_session_free (upf_proxy_session_t * ps)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  pool_put (pm->sessions, ps);
  if (CLIB_DEBUG)
    memset (ps, 0xfa, sizeof (*ps));
}

static void
proxy_session_lookup_add (session_t * s, upf_proxy_session_t * ps)
{
  upf_proxy_main_t *pm = &upf_proxy_main;

  vec_validate (pm->session_to_proxy_session[s->thread_index], s->session_index);
  pm->session_to_proxy_session[s->thread_index][s->session_index] =
    ps->session_index;
}

static void
proxy_session_lookup_del (session_t * s)
{
  upf_proxy_main_t *pm = &upf_proxy_main;

  pm->session_to_proxy_session[s->thread_index][s->session_index] = ~0;
}

static upf_proxy_session_t *
proxy_session_lookup (session_t * s)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  u32 ps_index;

  if (s->session_index < vec_len (pm->session_to_proxy_session[s->thread_index]))
    {
      ps_index = pm->session_to_proxy_session[s->thread_index][s->session_index];
      return proxy_session_get (ps_index);
    }
  return 0;
}

static void
active_open_session_lookup_add (session_t * s, upf_proxy_session_t * ps)
{
  upf_proxy_main_t *pm = &upf_proxy_main;

  vec_validate (pm->session_to_active_open_session[s->thread_index], s->session_index);
  pm->session_to_active_open_session[s->thread_index][s->session_index] =
    ps->session_index;
}

static void
active_open_session_lookup_del (session_t * s)
{
  upf_proxy_main_t *pm = &upf_proxy_main;

  pm->session_to_active_open_session[s->thread_index][s->session_index] = ~0;
}

static upf_proxy_session_t *
active_open_session_lookup (session_t * s)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  u32 ps_index;

  if (s->session_index < vec_len (pm->session_to_active_open_session[s->thread_index]))
    {
      ps_index = pm->session_to_active_open_session[s->thread_index][s->session_index];
      return proxy_session_get (ps_index);
    }
  return 0;
}

static session_t *
session_from_proxy_session_get (upf_proxy_session_t *ps, int is_active_open)
{
  if (!is_active_open)
    return session_get_if_valid (ps->proxy_session_index, ps->proxy_thread_index);
  else
    return session_get_if_valid (ps->active_open_session_index, ps->active_open_thread_index);
}

#define TCP_MSS 1460

static const char *upf_proxy_template =
  "HTTP/1.1 302 OK\r\n"
  "Location: %v\r\n"
  "Content-Type: text/html\r\n"
  "Cache-Control: private, no-cache, must-revalidate\r\n"
  "Expires: Mon, 11 Jan 1970 10:10:10 GMT\r\n"
  "Connection: close\r\n"
  "Pragma: no-cache\r\n" "Content-Length: %d\r\n\r\n%v";

static const char *http_error_template =
  "HTTP/1.1 %s\r\n"
  "Content-Type: text/html\r\n"
  "Cache-Control: private, no-cache, must-revalidate\r\n"
  "Expires: Mon, 11 Jan 1970 10:10:10 GMT\r\n"
  "Connection: close\r\n" "Pragma: no-cache\r\n" "Content-Length: 0\r\n\r\n";

static const char *wispr_proxy_template =
  "<!--\n"
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
  "<WISPAccessGatewayParam"
  " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
  " xsi:noNamespaceSchemaLocation=\"http://www.acmewisp.com/WISPAccessGatewayParam.xsd\">"
  "<Proxy>"
  "<MessageType>110</MessageType>"
  "<ResponseCode>200</ResponseCode>"
  "<NextURL>%v</NextURL>" "</Proxy>" "</WISPAccessGatewayParam>\n" "-->\n";

static const char *html_redirect_template =
  "<!DOCTYPE html>\n"
  "<html>\n"
  "%v"
  "   <head>\n"
  "      <title>Redirection</title>\n"
  "      <meta http-equiv=\"refresh\" content=\"0; URL=%v\">\n"
  "   </head>\n"
  "   <body>\n"
  "      Please <a href='%v'>click here</a> to continue\n"
  "   </body>\n" "</html>\n";

static void
http_redir_send_data (session_t * s, u8 * data)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  vnet_disconnect_args_t _a = { 0 }, *a = &_a;
  vlib_main_t *vm = vlib_get_main ();
  f64 last_sent_timer = vlib_time_now (vm);
  u32 offset, bytes_to_send;
  f64 delay = 10e-3;

  bytes_to_send = vec_len (data);
  offset = 0;

  while (bytes_to_send > 0)
    {
      int actual_transfer;

      actual_transfer = svm_fifo_enqueue
	(s->tx_fifo, bytes_to_send, data + offset);

      /* Made any progress? */
      if (actual_transfer <= 0)
	{
	  vlib_process_suspend (vm, delay);
	  /* 10s deadman timer */
	  if (vlib_time_now (vm) > last_sent_timer + 10.0)
	    {
	      a->handle = session_handle (s);
	      a->app_index = pm->server_app_index;
	      vnet_disconnect_session (a);
	      break;
	    }
	  /* Exponential backoff, within reason */
	  if (delay < 1.0)
	    delay = delay * 2.0;
	}
      else
	{
	  last_sent_timer = vlib_time_now (vm);
	  offset += actual_transfer;
	  bytes_to_send -= actual_transfer;

	  if (svm_fifo_set_event (s->tx_fifo))
	    session_send_io_evt_to_thread (s->tx_fifo,
					   SESSION_IO_EVT_TX_FLUSH);
	  delay = 10e-3;
	}
    }
}

static void
send_error (session_t * s, char *str)
{
  u8 *data;

  data = format (0, http_error_template, str);
  http_redir_send_data (s, data);
  vec_free (data);
}

static void
delete_proxy_session (session_t * s, int is_active_open)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  upf_proxy_session_t *ps = 0;
  vnet_disconnect_args_t _a, *a = &_a;
  session_t *active_open_session = 0;
  session_t *proxy_session = 0;

  proxy_server_sessions_writer_lock ();

  if (is_active_open)
    {
      ps = active_open_session_lookup (s);
      if (ps)
	{
	  active_open_session = s;
	  proxy_session = session_from_proxy_session_get (ps, 0);
	}
    }
  else
    {
      ps = proxy_session_lookup (s);
      if (!ps)
	{
	  u64 handle = session_handle (s);
	  clib_warning ("proxy session for %s handle %lld (%llx) AWOL",
			is_active_open ? "active open" : "server",
			handle, handle);
	}
      else
	{
	  proxy_session = s;
	  active_open_session = session_from_proxy_session_get (ps, 1);
	}
    }

  if (ps)
    proxy_session_free (ps);

  if (active_open_session)
    {
      a->handle = session_handle (active_open_session);
      a->app_index = pm->active_open_app_index;
      active_open_session_lookup_del (s);
      vnet_disconnect_session (a);
    }

  if (proxy_session)
    {
      a->handle = session_handle (proxy_session);
      a->app_index = pm->server_app_index;
      proxy_session_lookup_del (s);
      vnet_disconnect_session (a);
    }

  proxy_server_sessions_writer_unlock ();
}

static int
proxy_accept_callback (session_t * s)
{
  upf_proxy_session_t *ps;

  proxy_server_sessions_writer_lock ();

  ps = proxy_session_alloc (s->thread_index);
  proxy_session_lookup_add (s, ps);

  ps->rx_fifo = s->rx_fifo;
  ps->tx_fifo = s->tx_fifo;
  ps->proxy_session_index = s->session_index;
  ps->proxy_thread_index = s->thread_index;

  ps->flow_index = s->opaque & ~0x80000000;
  ps->is_reverse = !!(s->opaque & 0x80000000);

  //TBDps->session_state = PROXY_STATE_ESTABLISHED;
  //TBD proxy_server_session_timer_start (ps);

  proxy_server_sessions_writer_unlock ();

  s->session_state = SESSION_STATE_READY;
  return 0;
}

static void
proxy_disconnect_callback (session_t * s)
{
  delete_proxy_session (s, 0 /* is_active_open */ );
}

static void
proxy_reset_callback (session_t * s)
{
  clib_warning ("Reset session %U", format_session, s, 2);
  delete_proxy_session (s, 0 /* is_active_open */ );
}

static int
proxy_connected_callback (u32 app_index, u32 api_context,
			  session_t * s, u8 is_fail)
{
  clib_warning ("called...");
  return -1;
}

static int
proxy_add_segment_callback (u32 client_index, u64 segment_handle)
{
  clib_warning ("called...");
  return -1;
}

static int
proxy_rx_request (upf_proxy_session_t * ps)
{
  u32 max_dequeue, cursize;
  int n_read;

  cursize = vec_len (ps->rx_buf);
  max_dequeue = svm_fifo_max_dequeue_cons (ps->rx_fifo);
  if (PREDICT_FALSE (max_dequeue == 0))
    return -1;

  vec_validate (ps->rx_buf, cursize + max_dequeue - 1);
  n_read = app_recv_stream_raw (ps->rx_fifo, ps->rx_buf + cursize,
				max_dequeue, 0, 0 /* peek */ );
  ASSERT (n_read == max_dequeue);
  if (svm_fifo_is_empty_cons (ps->rx_fifo))
    svm_fifo_unset_event (ps->rx_fifo);

  _vec_len (ps->rx_buf) = cursize + n_read;
  return 0;
}

static int
proxy_rx_callback_static (session_t * s, upf_proxy_session_t * ps)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  flowtable_main_t *fm = &flowtable_main;
  vnet_disconnect_args_t _a = { 0 }, *a = &_a;
  upf_main_t *gtm = &upf_main;
  upf_session_t *sx;
  struct rules *active;
  flow_entry_t *flow;
  upf_pdr_t *pdr;
  upf_far_t *far;
  u8 *request = 0;
  u8 *wispr, *html, *http, *url;
  int i;
  int rv;

  rv = proxy_rx_request (ps);
  if (rv)
    return rv;

  request = ps->rx_buf;
  if (vec_len (request) < 6)
    {
      send_error (s, "400 Bad Request");
      goto out;
    }

  for (i = 0; i < vec_len (request) - 4; i++)
    {
      if (request[i] == 'G' &&
	  request[i + 1] == 'E' &&
	  request[i + 2] == 'T' && request[i + 3] == ' ')
	goto found;
    }
  send_error (s, "400 Bad Request");
  goto out;

found:
  flow = pool_elt_at_index (fm->flows, ps->flow_index);
  sx = pool_elt_at_index (gtm->sessions, flow->session_index);
  active = pfcp_get_rules (sx, PFCP_ACTIVE);
  pdr = pfcp_get_pdr_by_id (active, flow->pdr_id[ps->is_reverse]);
  far = pfcp_get_far_by_id (active, pdr->far_id);

  /* Send it */
  url = far->forward.redirect_information.uri;
  wispr = format (0, wispr_proxy_template, url);
  html = format (0, html_redirect_template, wispr, url, url);
  http = format (0, upf_proxy_template, url, vec_len (html), html);

  http_redir_send_data (s, http);

  vec_free (http);
  vec_free (html);
  vec_free (wispr);

out:
  /* Cleanup */
  vec_free (request);
  ps->rx_buf = request;

  a->handle = session_handle (s);
  a->app_index = pm->server_app_index;
  vnet_disconnect_session (a);

  return 0;
}

static int
proxy_rx_callback (session_t * s)
{
  u32 thread_index = vlib_get_thread_index ();
  upf_proxy_session_t *ps;

  ASSERT (s->thread_index == thread_index);

  proxy_server_sessions_reader_lock ();

  ps = proxy_session_lookup (s);
  if (!ps)
    {
      proxy_server_sessions_reader_unlock ();
      return -1;
    }

#if TBD
  if (ps->session_state == PROXY_STATE_FORWARDING)
    {
      svm_fifo_t *ao_tx_fifo;

      proxy_server_sessions_reader_unlock ();

      ao_tx_fifo = ps->rx_fifo;

      /*
       * Send event for active open tx fifo
       */
      if (svm_fifo_set_event (ao_tx_fifo))
	{
	  u32 ao_thread_index = ao_tx_fifo->master_thread_index;
	  u32 ao_session_index = ao_tx_fifo->master_session_index;
	  if (session_send_io_evt_to_thread_custom (&ao_session_index,
						    ao_thread_index,
						    SESSION_IO_EVT_TX))
	    clib_warning ("failed to enqueue tx evt");
	}

      if (svm_fifo_max_enqueue (ao_tx_fifo) <= TCP_MSS)
	svm_fifo_add_want_deq_ntf (ao_tx_fifo, SVM_FIFO_WANT_DEQ_NOTIF);
    }
  else
#endif
    {
      proxy_server_sessions_reader_unlock ();

      return proxy_rx_callback_static (s, ps);
#if 0
      int actual_transfer __attribute__((unused));
      vnet_connect_args_t _a, *a = &_a;
      svm_fifo_t *tx_fifo, *rx_fifo;
      upf_proxy_session_t *ps;
      int proxy_index;
      u32 max_dequeue;

      rx_fifo = s->rx_fifo;
      tx_fifo = s->tx_fifo;

      ASSERT (rx_fifo->master_thread_index == thread_index);
      ASSERT (tx_fifo->master_thread_index == thread_index);

      max_dequeue = svm_fifo_max_dequeue (s->rx_fifo);

      if (PREDICT_FALSE (max_dequeue == 0))
	return 0;

      actual_transfer = svm_fifo_peek (rx_fifo, 0 /* relative_offset */ ,
				       max_dequeue, pm->rx_buf[thread_index]);

      /* $$$ your message in this space: parse url, etc. */

      clib_memset (a, 0, sizeof (*a));

      clib_spinlock_lock_if_init (&pm->sessions_lock);
      pool_get (pm->sessions, ps);
      clib_memset (ps, 0, sizeof (*ps));
      ps->rx_fifo = rx_fifo;
      ps->tx_fifo = tx_fifo;
      ps->vpp_server_handle = session_handle (s);

      proxy_index = ps - pm->sessions;

      hash_set (pm->session_by_server_handle, ps->vpp_server_handle,
		proxy_index);

      clib_spinlock_unlock_if_init (&pm->sessions_lock);

      a->uri = (char *) pm->client_uri;
      a->api_context = proxy_index;
      a->app_index = pm->active_open_app_index;
      proxy_call_main_thread (a);
#endif
    }

  return 0;
}

static session_cb_vft_t proxy_session_cb_vft = {
  .session_accept_callback = proxy_accept_callback,
  .session_disconnect_callback = proxy_disconnect_callback,
  .session_connected_callback = proxy_connected_callback,
  .add_segment_callback = proxy_add_segment_callback,
  .builtin_app_rx_callback = proxy_rx_callback,
  .session_reset_callback = proxy_reset_callback
};

static int
active_open_connected_callback (u32 app_index, u32 opaque,
				session_t * s, u8 is_fail)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  upf_proxy_session_t *ps;
  u8 thread_index = vlib_get_thread_index ();

  if (is_fail)
    {
      clib_warning ("connection %d failed!", opaque);
      return 0;
    }

  /*
   * Setup proxy session handle.
   */
  proxy_server_sessions_writer_lock ();

  ps = pool_elt_at_index (pm->sessions, opaque);
  ps->active_open_session_index = s->session_index;
  ps->active_open_thread_index = s->thread_index;

  s->tx_fifo = ps->rx_fifo;
  s->rx_fifo = ps->tx_fifo;

  /*
   * Reset the active-open tx-fifo master indices so the active-open session
   * will receive data, etc.
   */
  s->tx_fifo->master_session_index = s->session_index;
  s->tx_fifo->master_thread_index = s->thread_index;

  /*
   * Account for the active-open session's use of the fifos
   * so they won't disappear until the last session which uses
   * them disappears
   */
  s->tx_fifo->refcnt++;
  s->rx_fifo->refcnt++;

  svm_fifo_init_ooo_lookup (s->tx_fifo, 1 /* deq ooo */ );
  svm_fifo_init_ooo_lookup (s->rx_fifo, 0 /* enq ooo */ );

  active_open_session_lookup_add (s, ps);

  proxy_server_sessions_writer_unlock ();

  /*
   * Send event for active open tx fifo
   */
  ASSERT (s->thread_index == thread_index);
  if (svm_fifo_set_event (s->tx_fifo))
    session_send_io_evt_to_thread (s->tx_fifo, SESSION_IO_EVT_TX);

  return 0;
}

static void
active_open_reset_callback (session_t * s)
{
  delete_proxy_session (s, 1 /* is_active_open */ );
}

static int
active_open_create_callback (session_t * s)
{
  return 0;
}

static void
active_open_disconnect_callback (session_t * s)
{
  delete_proxy_session (s, 1 /* is_active_open */ );
}

static int
active_open_rx_callback (session_t * s)
{
  svm_fifo_t *proxy_tx_fifo;

  proxy_tx_fifo = s->rx_fifo;

  /*
   * Send event for server tx fifo
   */
  if (svm_fifo_set_event (proxy_tx_fifo))
    {
      u8 thread_index = proxy_tx_fifo->master_thread_index;
      u32 session_index = proxy_tx_fifo->master_session_index;
      return session_send_io_evt_to_thread_custom (&session_index,
						   thread_index,
						   SESSION_IO_EVT_TX);
    }

  if (svm_fifo_max_enqueue (proxy_tx_fifo) <= TCP_MSS)
    svm_fifo_add_want_deq_ntf (proxy_tx_fifo, SVM_FIFO_WANT_DEQ_NOTIF);

  return 0;
}

/* *INDENT-OFF* */
static session_cb_vft_t active_open_clients = {
  .session_reset_callback = active_open_reset_callback,
  .session_connected_callback = active_open_connected_callback,
  .session_accept_callback = active_open_create_callback,
  .session_disconnect_callback = active_open_disconnect_callback,
  .builtin_app_rx_callback = active_open_rx_callback
};
/* *INDENT-ON* */

static int
proxy_server_attach ()
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  u64 options[APP_OPTIONS_N_OPTIONS];
  vnet_app_attach_args_t _a, *a = &_a;
  u32 segment_size = 128 << 20;
  int r = 0;

  clib_memset (a, 0, sizeof (*a));
  clib_memset (options, 0, sizeof (options));

  if (pm->private_segment_size)
    segment_size = pm->private_segment_size;
  a->name = format (0, "upf-proxy-server");
  a->api_client_index = pm->server_client_index;
  a->session_cb_vft = &proxy_session_cb_vft;
  a->options = options;
  a->options[APP_OPTIONS_SEGMENT_SIZE] = segment_size;
  a->options[APP_OPTIONS_RX_FIFO_SIZE] = pm->fifo_size;
  a->options[APP_OPTIONS_TX_FIFO_SIZE] = pm->fifo_size;
  a->options[APP_OPTIONS_PRIVATE_SEGMENT_COUNT] = pm->private_segment_count;
  a->options[APP_OPTIONS_PREALLOC_FIFO_PAIRS] =
    pm->prealloc_fifos ? pm->prealloc_fifos : 0;

  a->options[APP_OPTIONS_FLAGS] = APP_OPTIONS_FLAGS_IS_BUILTIN;

  if (vnet_application_attach (a))
    {
      clib_warning ("failed to attach server");
      r = -1;
    }
  else
    pm->server_app_index = a->app_index;

  vec_free (a->name);
  return r;
}

static int
active_open_attach (void)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  vnet_app_attach_args_t _a, *a = &_a;
  u64 options[16];
  int r = 0;

  clib_memset (a, 0, sizeof (*a));
  clib_memset (options, 0, sizeof (options));

  a->api_client_index = pm->active_open_client_index;
  a->session_cb_vft = &active_open_clients;
  a->name = format (0, "upf-proxy-active-open");

  options[APP_OPTIONS_ACCEPT_COOKIE] = 0x12345678;
  options[APP_OPTIONS_SEGMENT_SIZE] = 128 << 20;
  options[APP_OPTIONS_RX_FIFO_SIZE] = pm->fifo_size;
  options[APP_OPTIONS_TX_FIFO_SIZE] = pm->fifo_size;
  options[APP_OPTIONS_PRIVATE_SEGMENT_COUNT] = pm->private_segment_count;
  options[APP_OPTIONS_PREALLOC_FIFO_PAIRS] =
    pm->prealloc_fifos ? pm->prealloc_fifos : 0;

  options[APP_OPTIONS_FLAGS] = APP_OPTIONS_FLAGS_IS_BUILTIN
    | APP_OPTIONS_FLAGS_IS_PROXY;

  a->options = options;

  if (vnet_application_attach (a))
    r = -1;
  else
    pm->active_open_app_index = a->app_index;

  vec_free (a->name);
  return r;
}

#if TBD
/* create per fib listening socket for the HTTP redirect server */
static int
proxy_server_listen (u32 fib_index, int is_ip4)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  session_endpoint_cfg_t *cfg;
  vnet_listen_args_t _a, *a = &_a;
  app_listener_t *al;
  //session_t *ls;

  clib_memset (a, 0, sizeof (*a));
  a->app_index = pm->server_app_index;

  cfg = &a->sep_ext;
  *cfg = (session_endpoint_cfg_t) SESSION_ENDPOINT_CFG_NULL;
  cfg->is_ip4 = is_ip4;
  cfg->transport_proto = TRANSPORT_PROTO_TCP;
  cfg->peer.fib_index = fib_index;
  cfg->fib_index = fib_index;
  cfg->app_wrk_index = 0;

  if (vnet_listen (a))
    {
      clib_warning ("failed to start listen");
      return 1;
    }

  al = app_listener_get_w_handle (a->handle);
  //ls = app_listener_get_session (al);

  if (is_ip4)
    {
      vec_validate_init_empty (pm->ip4_listen_session_by_fib_index,
			       fib_index, 0);
      //pm->ip4_listen_session_by_fib_index[fib_index] = ls->connection_index;
      pm->ip4_listen_session_by_fib_index[fib_index] = al->session_index;
    }
  else
    {
      vec_validate_init_empty (pm->ip6_listen_session_by_fib_index,
			       fib_index, 0);
      //pm->ip6_listen_session_by_fib_index[fib_index] = ls->connection_index;
      pm->ip4_listen_session_by_fib_index[fib_index] = al->session_index;
    }

  return 0;
}
#endif

static int
proxy_create (vlib_main_t * vm, u32 fib_index, int is_ip4)
{
  upf_proxy_main_t *pm = &upf_proxy_main;
  vlib_thread_main_t *vtm = vlib_get_thread_main ();
  u32 num_threads;
  int i, rv;

  if (PREDICT_FALSE (pm->server_client_index == (u32) ~ 0))
    {
      num_threads = 1 /* main thread */  + vtm->n_threads;
      vec_validate (pm->server_event_queue, num_threads - 1);
      vec_validate (pm->active_open_event_queue, num_threads - 1);
      vec_validate (pm->session_to_proxy_session, num_threads - 1);
      vec_validate (pm->session_to_active_open_session, num_threads - 1);

      clib_rwlock_init (&pm->sessions_lock);

      if ((rv = proxy_server_attach ()))
	{
	  assert (rv == 0);
	  clib_warning ("failed to attach server app");
	  return -1;
	}
      if ((rv = active_open_attach ()))
	{
	  assert (rv == 0);
	  clib_warning ("failed to attach active open app");
	  return -1;
	}

      for (i = 0; i < num_threads; i++)
	{
	  pm->active_open_event_queue[i] =
	    session_main_get_vpp_event_queue (i);

	  ASSERT (pm->active_open_event_queue[i]);

	  pm->server_event_queue[i] = session_main_get_vpp_event_queue (i);
	}
    }

  return 0;
}

static void
upf_proxy_create (u32 fib_index, int is_ip4)
{
  vlib_main_t *vm = &vlib_global_main;
  upf_proxy_main_t *pm = &upf_proxy_main;
  int rv;

  if (pm->server_client_index == (u32) ~ 0)
    vnet_session_enable_disable (vm, 1 /* turn on TCP, etc. */ );

  rv = proxy_create (vm, fib_index, is_ip4);
  if (rv != 0)
      clib_error ("UPF http redirect server create returned %d", rv);
}

clib_error_t *
upf_proxy_main_init (vlib_main_t * vm)
{
  upf_proxy_main_t *pm = &upf_proxy_main;

  pm->fifo_size = 64 << 10;
  pm->rcv_buffer_size = 1024;
  pm->prealloc_fifos = 0;
  pm->private_segment_count = 0;
  pm->private_segment_size = 0;

  pm->server_client_index = ~0;
  pm->active_open_client_index = ~0;

  upf_proxy_create (0, 1);

  return 0;
}

VLIB_INIT_FUNCTION (upf_proxy_main_init);

/*
* fd.io coding-style-patch-verification: ON
*
* Local Variables:
* eval: (c-set-style "gnu")
* End:
*/
