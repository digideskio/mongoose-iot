// Copyright (c) 2014 Cesanta Software Limited
// All rights reserved
//
// This software is dual-licensed: you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation. For the terms of this
// license, see <http://www.gnu.org/licenses/>.
//
// You are free to use this software under the terms of the GNU General
// Public License, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// Alternatively, you can license this software under a commercial
// license, as set out in <http://cesanta.com/>.

#include <ctype.h>
#include "net_skeleton.h"
#include "v7.h"

static int s_received_signal = 0;
static const char *s_script_file_name = NULL;
static struct v7 *s_v7 = NULL;

static void signal_handler(int sig_num) {
  signal(sig_num, signal_handler);
  s_received_signal = sig_num;
}

static struct ns_connection *get_nc(struct v7_val *obj) {
  struct v7_val *p = v7_get(obj, "nc");
  return (p == NULL || v7_type(p) != V7_TYPE_NUM) ? NULL :
    (struct ns_connection *) (unsigned long) v7_number(p);
}

static enum v7_err js_send(struct v7_c_func_arg *cfa) {
  struct ns_connection *nc = get_nc(cfa->this_obj);
  char *p, buf[1024];  // TODO(lsm): fix possible truncation
  int i;

  if (nc == NULL) return V7_ERROR;

  for (i = 0; i < cfa->num_args; i++) {
    p = v7_stringify(cfa->args[i], buf, sizeof(buf));
    ns_send(nc, p, (int) strlen(p));
    if (p != buf) free(p);
  }
  return V7_OK;
}

static enum v7_err js_discard(struct v7_c_func_arg *cfa) {
  struct ns_connection *nc = get_nc(cfa->this_obj);
  double arg = v7_number(cfa->args[0]);

  if (cfa->num_args == 1 && v7_type(cfa->args[0]) == V7_TYPE_NUM &&
      arg > 0 && arg <= nc->recv_iobuf.len) {
    iobuf_remove(&nc->recv_iobuf, (size_t) arg);
  }

  return V7_OK;
}

static void init_js_conn(struct ns_connection *nc) {
  struct v7_val *js_srv = (struct v7_val *) nc->server->server_data;
  struct v7_val *js_conns = v7_get(js_srv, "connections");
  struct v7_val *js_conn = v7_push_new_object(s_v7);
  char key[40];

  if (js_conn != NULL && js_conns != NULL) {
    v7_set(s_v7, js_conn, "server", js_srv);
    v7_set(s_v7, js_conn, "nc", v7_push_number(s_v7, (unsigned long) nc));
    v7_set(s_v7, js_conn, "send", v7_push_func(s_v7, js_send));
    v7_set(s_v7, js_conn, "discard", v7_push_func(s_v7, js_discard));
    nc->connection_data = js_conn;        // Memorize JS connection

    // It's important to add js_conn to some object, otherwise it's ref_count
    // will remain 0 and v7 will wipe it out at next callback
    snprintf(key, sizeof(key), "%lu", (unsigned long) nc);
    v7_set(s_v7, js_conns, key, js_conn);
  } else {
    // Failed to create JS connection object, close
    nc->flags |= NSF_CLOSE_IMMEDIATELY;
  }
}

static void free_js_conn(struct ns_connection *nc) {
  struct v7_val *js_srv = (struct v7_val *) nc->server->server_data;
  struct v7_val *js_conns = v7_get(js_srv, "connections");
  struct v7_val *js_conn = (struct v7_val *) nc->connection_data;
  char key[40];

  if (js_conn != NULL && js_conns != NULL) {
    snprintf(key, sizeof(key), "%lu", (unsigned long) nc);
    v7_del(s_v7, js_conns, key);
    nc->connection_data = NULL;
  }
}

static void call_handler(struct ns_connection *nc, const char *name) {
  struct iobuf *io = &nc->recv_iobuf;
  struct v7_val *js_srv = (struct v7_val *) nc->server->server_data;
  struct v7_val *js_conn = (struct v7_val *) nc->connection_data;
  struct v7_val *js_handler, *data, *res;

  if (js_conn != NULL && (js_handler = v7_get(js_srv, name)) != NULL) {
    data = v7_push_string(s_v7, io->buf, io->len, 0);
    v7_set(s_v7, js_conn, "data", data);

    // Push JS event handler and it's argument, JS connection, on stack
    v7_push_val(s_v7, js_handler);
    v7_push_val(s_v7, js_conn);

    // Call the handler
    if ((res = v7_call(s_v7, js_srv, 1)) == NULL) {
      fprintf(stderr, "Error executing %s handler: %s\n",
              name, v7_get_error_string(s_v7));
    } else {
      // If handler returns true, then close the connection
      if (v7_is_true(res)) {
        nc->flags |= NSF_FINISHED_SENDING_DATA;
      }
    }
  }
}

static void tcp_handler(struct ns_connection *nc, enum ns_event ev, void *p) {
  (void) p;
  switch (ev) {
    case NS_ACCEPT: init_js_conn(nc); call_handler(nc, "onaccept"); break;
    case NS_RECV: call_handler(nc, "onmessage"); break;
    case NS_POLL: call_handler(nc, "onpoll"); break;
    case NS_CLOSE: call_handler(nc, "onclose"); free_js_conn(nc); break;
    default: break;
  }
}

static enum v7_err js_run(struct v7_c_func_arg *cfa) {
  struct v7_val *js_srv = v7_get(cfa->this_obj, "srv");
  struct v7_val *onstart = v7_get(cfa->this_obj, "onstart");
  time_t mtime = 0, cur_time, prev_time = 0;
  struct stat st;

  if (js_srv != NULL) {
    struct ns_server *srv = (struct ns_server *)
      (unsigned long) v7_number(js_srv);

    // Call "onstart" handler if it is defined
    if (onstart != NULL) {
      //int old_sp = v7_sp(cfa->v7);
      v7_push_val(cfa->v7, onstart);
      v7_call(cfa->v7, cfa->this_obj, 0);
      //v7_pop(cfa->v7, v7_sp(cfa->v7) - old_sp);
    }

    // Enter listening loop
    while (srv->listening_sock != INVALID_SOCKET && s_received_signal == 0) {
      ns_server_poll(srv, 200);
      if ((cur_time = time(NULL)) > prev_time) {
        prev_time = cur_time;
        if (stat(s_script_file_name, &st) == 0 && mtime != 0 &&
            mtime != st.st_mtime) break;
        mtime = st.st_mtime;
      }
    }
    ns_server_free(srv);
  }

  return V7_OK;
}

static enum v7_err js_net(struct v7_c_func_arg *cfa) {
  struct v7_val *listening_port, *v, *js_srv;
  struct ns_server *srv;
  char *s, buf[100];

  if (cfa->num_args < 1 || v7_type(cfa->args[0]) != V7_TYPE_OBJ ||
      (listening_port = v7_get(cfa->args[0], "listening_port")) == NULL)
        return V7_ERROR;

  // Set up javascript object that represents a server
  js_srv = v7_push_new_object(cfa->v7);
  v7_copy(cfa->v7, cfa->args[0], js_srv);
  v = v7_push_new_object(cfa->v7);
  v7_set(cfa->v7, js_srv, "connections", v);
  v7_set(cfa->v7, js_srv, "run", v7_push_func(cfa->v7, js_run));

  // Initialize net skeleton TCP server and bind it to the JS object
  // by setting 'srv' property, which is a "struct ns_server *"
  srv = (struct ns_server *) calloc(1, sizeof(*srv));
  ns_server_init(srv, js_srv, tcp_handler);
  v = v7_push_number(cfa->v7, (unsigned long) srv);
  v7_set(cfa->v7, js_srv, "srv", v);

  s = v7_stringify(listening_port, buf, sizeof(buf));
  ns_bind(srv, s);
  if (s != buf) free(s);

  // Push result on stack
  v7_push_val(cfa->v7, js_srv);

  return V7_OK;
}

static void cleanup(void) {
  fprintf(stderr, "Terminating on signal %d\n", s_received_signal);
  v7_destroy(&s_v7);
}

static void run_script(const char *file_name) {
  int error_code;

  s_v7 = v7_create();
  v7_set(s_v7, v7_rootns(s_v7), "NetEventManager", v7_push_func(s_v7, js_net));

  if ((error_code = v7_exec_file(s_v7, file_name)) != V7_OK) {
    fprintf(stderr, "%s\n", v7_get_error_string(s_v7));
  }
  if (s_received_signal == 0) {
    sleep(1);
  }
  v7_destroy(&s_v7);
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <js_script_file>\n", argv[0]);
    return EXIT_FAILURE;
  }
  s_script_file_name = argv[1];

  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);
  atexit(cleanup);

  while (s_received_signal == 0) {
    run_script(s_script_file_name);
  }

  return EXIT_SUCCESS;
}
