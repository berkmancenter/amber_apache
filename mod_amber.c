/* Include the required headers from httpd */
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"
#include "http_log.h"

/* Define prototypes of our functions in this module */
static void register_hooks(apr_pool_t *pool);
static int amber_handler(request_rec *r);
static apr_status_t amber_filter(ap_filter_t *f, apr_bucket_brigade *bb);
static int should_apply_filter(ap_filter_t *f);

/* Define our module as an entity and assign a function for registering hooks  */

module AP_MODULE_DECLARE_DATA   amber_module =
{
    STANDARD20_MODULE_STUFF,
    NULL,            // Per-directory configuration handler
    NULL,            // Merge handler for per-directory configurations
    NULL,            // Per-server configuration handler
    NULL,            // Merge handler for per-server configurations
    NULL,            // Any directives we may have for httpd
    register_hooks   // Our hook registering function
};


/* register_hooks: Adds a hook to the httpd process */
static void register_hooks(apr_pool_t *pool) 
{
    
    /* Hook the request handler */
    ap_hook_handler(amber_handler, NULL, NULL, APR_HOOK_LAST);
    ap_register_output_filter("amber-filter", amber_filter, NULL, AP_FTYPE_RESOURCE) ;

}

/* Handler - TODO: Remove */
static int amber_handler(request_rec *r)
{
    /* First off, we need to check if this is a call for the "example" handler.
     * If it is, we accept it and do our things, it not, we simply return DECLINED,
     * and Apache will try somewhere else.
     */
    if (!r->handler || strcmp(r->handler, "amber-handler")) return (DECLINED);
    
    // The first thing we will do is write a simple "Hello, world!" back to the client.
    ap_rputs("Hello, world!<br/>", r);
    return OK;
}

/** 
 * Search for external links to be annotated with cache location or saved for future caching
 * @param f the filter
 * @param bb the bucket brigade of content to be examined
 * @return status code
 */
static apr_status_t amber_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    apr_bucket *bucket, *next_bucket;
    apr_bucket_brigade *outBB;
    apr_status_t rv;

    ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Filter start");

    if (!should_apply_filter(f)) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Skipping file");
        return ap_pass_brigade(f->next, bb);
    }

    /* This should never happen */
    if (APR_BRIGADE_EMPTY(bb)) {
        return APR_SUCCESS;
    }


    
    /* Create the output brigade which will contain the transformed response */
    if (!(outBB = apr_brigade_create(f->r->pool, f->c->bucket_alloc))) {
        /* Log the problem and don't process the brigade */
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Could not create output buffer brigade");    
        return ap_pass_brigade(f->next, bb);
    } 

    bucket = APR_BRIGADE_FIRST(bb);

    /* Main loop through which we process all the buckets in the brigade */
    while (bucket  != APR_BRIGADE_SENTINEL(bb)) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "In bucket loop");
        next_bucket = APR_BUCKET_NEXT(bucket);

        /* This is a metadata bucket indicating the end of the brigade */
        if (APR_BUCKET_IS_EOS(bucket)) {
            APR_BUCKET_REMOVE(bucket);
            APR_BRIGADE_INSERT_TAIL(outBB, bucket);
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Filter end");
            return ap_pass_brigade(f->next, outBB);
        }

        /* This is a metadata bucket indicating that we should flush output, and then continue */
        if (APR_BUCKET_IS_FLUSH(bucket)) {
            APR_BRIGADE_INSERT_TAIL(outBB, bucket);
            rv = ap_pass_brigade(f->next, outBB);
            if (rv != APR_SUCCESS) {
                return rv;
            }
            /* Create a new output brigade to work with */
            if (!(outBB = apr_brigade_create(f->r->pool, f->c->bucket_alloc))) {
                /* Log the problem and don't process the rest of the brigade */
                ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Could not create output buffer brigade after flush");    
                return ap_pass_brigade(f->next, bb);
            }
            continue;
        }

        // Do stuff

        APR_BUCKET_REMOVE(bucket);

        // We would actually insert our new bucket here

        APR_BRIGADE_INSERT_TAIL(outBB, bucket); 

        bucket = next_bucket;
    }

    ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Filter end");

    return ap_pass_brigade(f->next, outBB);
}    

/**
 * Determine whether or not our filter should process this request. We only want to process HTML files
 * @param f the filter
 * @return true if the requests should be processed
 */
static int should_apply_filter(ap_filter_t *f) {
    return (f && f->r && f->r->content_type && !strcmp("text/html", f->r->content_type));
}

