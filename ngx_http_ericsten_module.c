/*

Author:
    Eric Stenson (ericsten) Oct 2017

Module Description:
    Test HTTP rewrite module to exercise nginx thread pools.

    The rewrite handler entry point, ngx_http_ericsten_handler, performs its
    blocking operations on a task that is queued to a thread pool.

    Upon completion of the task, the request signals it is ready to resume
    processing by calling ngx_http_handler(ngx_http_request_t *r).

    As it turns out, nginx thread pools are only available on non-Windows
    platforms.  Therefore, a compile time assert is added to ensure
    compilation fails if the '--with-threads' was not used in ./configure.

*/

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#ifndef NGX_THREADS
#error ngx_http_ericsten_module.c requires --with-threads
#endif /* NGX_THREADS */

static ngx_int_t ngx_http_ericsten_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_ericsten_get_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_ericsten_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_ericsten_handler(ngx_http_request_t *r);

static void ngx_http_ericsten_dostuff(void *data, ngx_log_t *log);
static void ngx_http_ericsten_dostuff_completion_handler(ngx_event_t *ev);

static ngx_str_t ngx_ericsten_thread_pool_name = ngx_string("ericsten");

#define TRUE 1
#define FALSE 0

#define ngx_http_null_variable  { ngx_null_string, NULL, NULL, 0, 0, 0 }

#define MAX_VARIABLE_SIZE 64

typedef enum ERICSTEN_TASK_STATE_tag
{
    ES_TASK_INIT = 0,
    ES_TASK_PROCESSING,
    ES_TASK_DONE
} ERICSTEN_STATE;

char * ngx_ericsten_states[] =
{
    "INIT",
    "PROCESSING",
    "DONE",
    "INVALID"
};

//
// Per-request context.  This is effectively the "out-params" from the thread pool task.
//
typedef struct
{
    ERICSTEN_STATE      state;    
    int                 msSleep;        // Time the task slept while doing background work, in milliseconds.
    ngx_http_request_t *r;              // Http Request pointer, for the thread completion.
} ngx_http_ericsten_ctx_t;

//
// Per-task context.  This is effectively the "in-params" to the thread pool task.
//
typedef struct
{
    ngx_http_ericsten_ctx_t *ericsten_ctx;
    int                      random_value;
} ngx_http_ericsten_task_ctx_t;

static ngx_command_t  ngx_http_ericsten_commands[] = {
      ngx_null_command
};

static ngx_http_module_t  ngx_http_ericsten_module_ctx = {
    ngx_http_ericsten_add_variables,    /* preconfiguration */
    ngx_http_ericsten_init,             /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    NULL,                               /* create location configuration */
    NULL                                /* merge location configuration */
};

ngx_module_t  ngx_http_ericsten_module = {
    NGX_MODULE_V1,
    &ngx_http_ericsten_module_ctx,      /* module context */
    ngx_http_ericsten_commands,         /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};

enum ERICSTEN_VAR_INDEX
{
    ES_VAR_SLEEP = 0,
    ES_VAR_BANANA = 1
};

static ngx_http_variable_t  ngx_http_ericsten_vars[] = {

    { ngx_string("ericsten_sleep"), NULL, ngx_http_ericsten_get_variable,
      ES_VAR_SLEEP, NGX_HTTP_VAR_NOCACHEABLE, 0 },

    { ngx_string("ericsten_banana"), NULL, ngx_http_ericsten_get_variable,
      ES_VAR_BANANA, NGX_HTTP_VAR_NOCACHEABLE, 1 },

    ngx_http_null_variable
};

static ngx_int_t
ngx_http_ericsten_get_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char                   *p = NULL;
    ngx_http_ericsten_ctx_t  *ctx = ngx_http_get_module_ctx(r, ngx_http_ericsten_module);
    int                       found = FALSE;

    if (ctx == NULL)
    {
        goto Finished;
    }

    //
    // Allocate memory for return string.
    //

    p = ngx_pnalloc(r->pool, MAX_VARIABLE_SIZE);
    if (p == NULL) 
    {
        return NGX_ERROR;
    }

    switch (data)
    {
    case ES_VAR_SLEEP:

        //
        // Fetch the msSleep from the module's context.
        //

        found = TRUE;
        v->len = ngx_sprintf(p, "%uA", ctx->msSleep) - p;
        break;

   case ES_VAR_BANANA:

        //
        // Put the string "banana" in the variable.
        //

        found = TRUE;
        v->len = ngx_sprintf(p, "banana") - p;
        break;

    default:

        //
        // No idea: Just put a '0' in the variable.
        //

        found = TRUE;
        v->len = ngx_sprintf(p, "%u", 0) - p;
        break;
    }

Finished:
    if (found == TRUE)
    {
        v->valid = 1;
        v->no_cacheable = 1;
        v->not_found = 0;
        v->data = p;
    }
    else
    {
        v->valid = 0;
        v->no_cacheable = 1;
        v->not_found = 1;
        v->data = NULL;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_ericsten_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_ericsten_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_ericsten_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;
    ngx_thread_pool_t          *tp;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_ericsten_handler;

    //
    // Set up our thread pool.
    //

#if (NGX_THREADS)
    tp = ngx_thread_pool_add(cf, &ngx_ericsten_thread_pool_name);

    if (tp == NULL) {
        return NGX_ERROR;
    }
#else
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"aio threads\" "
                           "is unsupported on this platform");
        return NGX_ERROR;
#endif

    return NGX_OK;
}

static ngx_int_t
ngx_http_ericsten_handler(ngx_http_request_t *r)
{
    ngx_http_ericsten_ctx_t       *ctx = NULL;
    ngx_thread_pool_t             *tp = NULL;
    ngx_thread_task_t             *task = NULL;
    ngx_http_ericsten_task_ctx_t  *task_ctx = NULL;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "ngx_http_ericsten_handler: Entering rewrite handler");

    ctx = ngx_http_get_module_ctx(r, ngx_http_ericsten_module);
    if (ctx != NULL)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
           "ngx_http_ericsten_handler: Resuming a previously seen request. "
           "request_state: %s",
           ngx_ericsten_states[ctx->state]);

        //
        // If the thread pool task could fail, this would be the correct
        // point to fail the request and set a final response status.
        //
        // Alternately, if there were multiple tasks, this would be the place
        // to process the state machine on the per-request context and move to
        // the next task.
        //
    }
    else
    {
        //
        // Create a context for the module.
        //

        ctx = (ngx_http_ericsten_ctx_t*) ngx_pcalloc(r->pool, sizeof(ngx_http_ericsten_ctx_t));
        if (ctx == NULL) 
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        //
        // Set the context.
        //

        ngx_http_set_ctx(r, ctx, ngx_http_ericsten_module);

        //
        // Init the state on the context.
        //

        ctx->state = ES_TASK_INIT;
        ctx->msSleep = 0;
        ctx->r = r;

        //
        // Queue work item to a background thread & return NGX_AGAIN
        //

        tp = ngx_thread_pool_get((ngx_cycle_t *) ngx_cycle, &ngx_ericsten_thread_pool_name);
        if (tp == NULL)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
              "ngx_http_ericsten: thread pool \"%V\" not found", &ngx_ericsten_thread_pool_name);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        task = ngx_thread_task_alloc(r->connection->pool, sizeof(ngx_http_ericsten_task_ctx_t));
        if (task == NULL)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
              "ngx_http_ericsten: failed to alloc new task");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        task_ctx = task->ctx;
        task_ctx->ericsten_ctx = ctx;
        task_ctx->random_value = ngx_random();

        task->handler = ngx_http_ericsten_dostuff;
        task->event.handler = ngx_http_ericsten_dostuff_completion_handler;
        task->event.data = ctx;

        if (ngx_thread_task_post(tp, task) != NGX_OK)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "ngx_http_ericsten: failed to post new task");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        r->main->blocked++;
        r->aio = 1;

        return NGX_AGAIN;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "ngx_http_ericsten_handler: Finished rewrite handler.");

    return NGX_DECLINED;
}

//
// Thread Pool Task Functions
//

static void
ngx_http_ericsten_dostuff(void *data, ngx_log_t *log)
{
    ngx_http_ericsten_task_ctx_t  *task_ctx = data;
    ngx_http_ericsten_ctx_t       *ctx = task_ctx->ericsten_ctx;
    ngx_uint_t                     msec_sleep;

    ctx->state = ES_TASK_PROCESSING;

    //
    // Our blocking operation is simple: 
    // Sleep from 100 to 1000 milliseconds (in 100ms increments).
    // This uses the input parameter passed via the task context.
    //

    msec_sleep = (((task_ctx->random_value % 9) + 1) * 100);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->r->connection->log, 0,
        "ngx_http_ericsten_dostuff: About to sleep for %d msec", msec_sleep);
    ngx_msleep(msec_sleep);

    //
    // Any product of our processing that we need to pass back to the main
    // handler should be put on the per-request context.
    //

    ctx->msSleep = msec_sleep;
    ctx->state = ES_TASK_DONE;
}

static void
ngx_http_ericsten_dostuff_completion_handler(ngx_event_t *ev)
{
    ngx_connection_t         *c;
    ngx_http_request_t       *r;
    ngx_http_ericsten_ctx_t  *ctx = ev->data;
    
    r = ctx->r;
    c = r->connection;

    ngx_http_set_log_request(c->log, r);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
        "ngx_http_ericsten_dostuff_completion_handler: \"%V?%V\"", &r->uri, &r->args);

    //
    // The task completion handler executes on the main event loop, and is
    // pretty straightfoward: Mark the background processing complete, and
    // call the nginx HTTP function to resume processing of the request.
    //

    r->main->blocked--;
    r->aio = 0;

    ngx_http_handler(r);
}
