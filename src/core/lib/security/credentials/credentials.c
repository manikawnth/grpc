/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "src/core/lib/security/credentials/credentials.h"

#include <stdio.h>
#include <string.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/http/httpcli.h"
#include "src/core/lib/http/parser.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/json/json.h"
#include "src/core/lib/support/string.h"
#include "src/core/lib/surface/api_trace.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>

/* -- Common. -- */

grpc_credentials_metadata_request *grpc_credentials_metadata_request_create(
    grpc_call_credentials *creds) {
  grpc_credentials_metadata_request *r =
      gpr_zalloc(sizeof(grpc_credentials_metadata_request));
  r->creds = grpc_call_credentials_ref(creds);
  return r;
}

void grpc_credentials_metadata_request_destroy(
    grpc_exec_ctx *exec_ctx, grpc_credentials_metadata_request *r) {
  grpc_call_credentials_unref(exec_ctx, r->creds);
  grpc_http_response_destroy(&r->response);
  gpr_free(r);
}

grpc_channel_credentials *grpc_channel_credentials_ref(
    grpc_channel_credentials *creds) {
  if (creds == NULL) return NULL;
  gpr_ref(&creds->refcount);
  return creds;
}

void grpc_channel_credentials_unref(grpc_exec_ctx *exec_ctx,
                                    grpc_channel_credentials *creds) {
  if (creds == NULL) return;
  if (gpr_unref(&creds->refcount)) {
    if (creds->vtable->destruct != NULL) {
      creds->vtable->destruct(exec_ctx, creds);
    }
    gpr_free(creds);
  }
}

void grpc_channel_credentials_release(grpc_channel_credentials *creds) {
  GRPC_API_TRACE("grpc_channel_credentials_release(creds=%p)", 1, (creds));
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_channel_credentials_unref(&exec_ctx, creds);
  grpc_exec_ctx_finish(&exec_ctx);
}

grpc_call_credentials *grpc_call_credentials_ref(grpc_call_credentials *creds) {
  if (creds == NULL) return NULL;
  gpr_ref(&creds->refcount);
  return creds;
}

void grpc_call_credentials_unref(grpc_exec_ctx *exec_ctx,
                                 grpc_call_credentials *creds) {
  if (creds == NULL) return;
  if (gpr_unref(&creds->refcount)) {
    if (creds->vtable->destruct != NULL) {
      creds->vtable->destruct(exec_ctx, creds);
    }
    gpr_free(creds);
  }
}

void grpc_call_credentials_release(grpc_call_credentials *creds) {
  GRPC_API_TRACE("grpc_call_credentials_release(creds=%p)", 1, (creds));
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_call_credentials_unref(&exec_ctx, creds);
  grpc_exec_ctx_finish(&exec_ctx);
}

bool grpc_call_credentials_get_request_metadata(
    grpc_exec_ctx *exec_ctx, grpc_call_credentials *creds,
    grpc_polling_entity *pollent, grpc_auth_metadata_context context,
    grpc_credentials_mdelem_array *md_array, grpc_closure *on_request_metadata,
    grpc_error **error) {
  if (creds == NULL || creds->vtable->get_request_metadata == NULL) {
    return true;
  }
  return creds->vtable->get_request_metadata(
      exec_ctx, creds, pollent, context, md_array, on_request_metadata, error);
}

void grpc_call_credentials_cancel_get_request_metadata(
    grpc_exec_ctx *exec_ctx, grpc_call_credentials *creds,
    grpc_credentials_mdelem_array *md_array, grpc_error *error) {
  if (creds == NULL || creds->vtable->cancel_get_request_metadata == NULL) {
    return;
  }
  creds->vtable->cancel_get_request_metadata(exec_ctx, creds, md_array, error);
}

grpc_security_status grpc_channel_credentials_create_security_connector(
    grpc_exec_ctx *exec_ctx, grpc_channel_credentials *channel_creds,
    const char *target, const grpc_channel_args *args,
    grpc_channel_security_connector **sc, grpc_channel_args **new_args) {
  *new_args = NULL;
  if (channel_creds == NULL) {
    return GRPC_SECURITY_ERROR;
  }
  GPR_ASSERT(channel_creds->vtable->create_security_connector != NULL);
  return channel_creds->vtable->create_security_connector(
      exec_ctx, channel_creds, NULL, target, args, sc, new_args);
}

grpc_channel_credentials *
grpc_channel_credentials_duplicate_without_call_credentials(
    grpc_channel_credentials *channel_creds) {
  if (channel_creds != NULL && channel_creds->vtable != NULL &&
      channel_creds->vtable->duplicate_without_call_credentials != NULL) {
    return channel_creds->vtable->duplicate_without_call_credentials(
        channel_creds);
  } else {
    return grpc_channel_credentials_ref(channel_creds);
  }
}

static void credentials_pointer_arg_destroy(grpc_exec_ctx *exec_ctx, void *p) {
  grpc_channel_credentials_unref(exec_ctx, p);
}

static void *credentials_pointer_arg_copy(void *p) {
  return grpc_channel_credentials_ref(p);
}

static int credentials_pointer_cmp(void *a, void *b) { return GPR_ICMP(a, b); }

static const grpc_arg_pointer_vtable credentials_pointer_vtable = {
    credentials_pointer_arg_copy, credentials_pointer_arg_destroy,
    credentials_pointer_cmp};

grpc_arg grpc_channel_credentials_to_arg(
    grpc_channel_credentials *credentials) {
  return grpc_channel_arg_pointer_create(
      GRPC_ARG_CHANNEL_CREDENTIALS, credentials, &credentials_pointer_vtable);
}

grpc_channel_credentials *grpc_channel_credentials_from_arg(
    const grpc_arg *arg) {
  if (strcmp(arg->key, GRPC_ARG_CHANNEL_CREDENTIALS)) return NULL;
  if (arg->type != GRPC_ARG_POINTER) {
    gpr_log(GPR_ERROR, "Invalid type %d for arg %s", arg->type,
            GRPC_ARG_CHANNEL_CREDENTIALS);
    return NULL;
  }
  return arg->value.pointer.p;
}

grpc_channel_credentials *grpc_channel_credentials_find_in_args(
    const grpc_channel_args *args) {
  size_t i;
  if (args == NULL) return NULL;
  for (i = 0; i < args->num_args; i++) {
    grpc_channel_credentials *credentials =
        grpc_channel_credentials_from_arg(&args->args[i]);
    if (credentials != NULL) return credentials;
  }
  return NULL;
}

grpc_server_credentials *grpc_server_credentials_ref(
    grpc_server_credentials *creds) {
  if (creds == NULL) return NULL;
  gpr_ref(&creds->refcount);
  return creds;
}

void grpc_server_credentials_unref(grpc_exec_ctx *exec_ctx,
                                   grpc_server_credentials *creds) {
  if (creds == NULL) return;
  if (gpr_unref(&creds->refcount)) {
    if (creds->vtable->destruct != NULL) {
      creds->vtable->destruct(exec_ctx, creds);
    }
    if (creds->processor.destroy != NULL && creds->processor.state != NULL) {
      creds->processor.destroy(creds->processor.state);
    }
    gpr_free(creds);
  }
}

void grpc_server_credentials_release(grpc_server_credentials *creds) {
  GRPC_API_TRACE("grpc_server_credentials_release(creds=%p)", 1, (creds));
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_server_credentials_unref(&exec_ctx, creds);
  grpc_exec_ctx_finish(&exec_ctx);
}

grpc_security_status grpc_server_credentials_create_security_connector(
    grpc_exec_ctx *exec_ctx, grpc_server_credentials *creds,
    grpc_server_security_connector **sc) {
  if (creds == NULL || creds->vtable->create_security_connector == NULL) {
    gpr_log(GPR_ERROR, "Server credentials cannot create security context.");
    return GRPC_SECURITY_ERROR;
  }
  return creds->vtable->create_security_connector(exec_ctx, creds, sc);
}

void grpc_server_credentials_set_auth_metadata_processor(
    grpc_server_credentials *creds, grpc_auth_metadata_processor processor) {
  GRPC_API_TRACE(
      "grpc_server_credentials_set_auth_metadata_processor("
      "creds=%p, "
      "processor=grpc_auth_metadata_processor { process: %p, state: %p })",
      3, (creds, (void *)(intptr_t)processor.process, processor.state));
  if (creds == NULL) return;
  if (creds->processor.destroy != NULL && creds->processor.state != NULL) {
    creds->processor.destroy(creds->processor.state);
  }
  creds->processor = processor;
}

static void server_credentials_pointer_arg_destroy(grpc_exec_ctx *exec_ctx,
                                                   void *p) {
  grpc_server_credentials_unref(exec_ctx, p);
}

static void *server_credentials_pointer_arg_copy(void *p) {
  return grpc_server_credentials_ref(p);
}

static int server_credentials_pointer_cmp(void *a, void *b) {
  return GPR_ICMP(a, b);
}

static const grpc_arg_pointer_vtable cred_ptr_vtable = {
    server_credentials_pointer_arg_copy, server_credentials_pointer_arg_destroy,
    server_credentials_pointer_cmp};

grpc_arg grpc_server_credentials_to_arg(grpc_server_credentials *p) {
  return grpc_channel_arg_pointer_create(GRPC_SERVER_CREDENTIALS_ARG, p,
                                         &cred_ptr_vtable);
}

grpc_server_credentials *grpc_server_credentials_from_arg(const grpc_arg *arg) {
  if (strcmp(arg->key, GRPC_SERVER_CREDENTIALS_ARG) != 0) return NULL;
  if (arg->type != GRPC_ARG_POINTER) {
    gpr_log(GPR_ERROR, "Invalid type %d for arg %s", arg->type,
            GRPC_SERVER_CREDENTIALS_ARG);
    return NULL;
  }
  return arg->value.pointer.p;
}

grpc_server_credentials *grpc_find_server_credentials_in_args(
    const grpc_channel_args *args) {
  size_t i;
  if (args == NULL) return NULL;
  for (i = 0; i < args->num_args; i++) {
    grpc_server_credentials *p =
        grpc_server_credentials_from_arg(&args->args[i]);
    if (p != NULL) return p;
  }
  return NULL;
}
