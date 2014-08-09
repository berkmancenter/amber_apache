/* Include the required headers from httpd */
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"
#include "http_log.h"
#include "pcre.h"

/* Define our data types */
typedef struct {
    int   count;        /* Number of matching insertion positions and urls */
    int   *insert_pos;  /* Array - positions within the buffer where additional 
                           attributes should in inserted for matching hrefs */
    char  *url;         /* Array - urls within the buffer. */
} amber_matches_t;

/* Define prototypes of our functions in this module */
static void register_hooks(apr_pool_t *pool);
static int amber_handler(request_rec *r);
static apr_status_t amber_filter(ap_filter_t *f, apr_bucket_brigade *bb);
static int amber_should_apply_filter(ap_filter_t *f);
static apr_bucket* amber_process_bucket(ap_filter_t *f, apr_bucket *bucket, const char *buffer, size_t buffer_size);
static amber_matches_t find_links_in_bucket(ap_filter_t *f, apr_bucket *bucket);

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
    apr_bucket          *bucket, *next_bucket, *new_bucket;
    apr_bucket_brigade  *outBB;
    const char          *buffer;
    size_t              buffer_size;
    apr_read_type_e     read_mode;

    apr_status_t rv;

    ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Filter start");

    if (!amber_should_apply_filter(f)) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Skipping file");
        return ap_pass_brigade(f->next, bb);
    }

    /* This should never happen */
    if (APR_BRIGADE_EMPTY(bb)) {
        return APR_SUCCESS;
    }

    /* Create the output brigade which will contain the transformed response */
    /* TODO - Save this brigade in the context, so we don't create one for each invocation */    
    if (!(outBB = apr_brigade_create(f->r->pool, f->c->bucket_alloc))) {
        /* Error - log the problem and don't process the brigade */
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Could not create output buffer brigade");    
        return ap_pass_brigade(f->next, bb);
    } 


    /* Start out with non-blocking reads, so that if we're reading from a stream, we don't 
       unecessarily block until the stream is complete */     
    read_mode = APR_NONBLOCK_READ; 
    bucket = APR_BRIGADE_FIRST(bb);

    /* Main loop through which we process all the buckets in the brigade */
    while (bucket != APR_BRIGADE_SENTINEL(bb)) {
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
            if ((rv = ap_pass_brigade(f->next, outBB)) != APR_SUCCESS) {
                return rv;
            }
            /* Reset our output brigade */
            apr_brigade_cleanup(outBB);
            continue;
        }

        /* Read the bucket! */
        if (!APR_BUCKET_IS_METADATA(bucket)) {
            rv = apr_bucket_read(bucket, &buffer, &buffer_size, read_mode);
            if (APR_SUCCESS == rv) {
                read_mode = APR_NONBLOCK_READ;
                new_bucket = amber_process_bucket(f, bucket, buffer, buffer_size);
            } else if ((APR_EAGAIN == rv) && (APR_NONBLOCK_READ == read_mode)) {
                /* Data is not available, so we need to try again. Flush everything we have so far 
                   and switch to using blocking reads */
                read_mode = APR_BLOCK_READ;
                APR_BRIGADE_INSERT_TAIL(outBB, apr_bucket_flush_create(f->c->bucket_alloc));
                if ((rv = ap_pass_brigade(f->next, outBB)) != APR_SUCCESS) {
                    return rv;
                }
                apr_brigade_cleanup(outBB);
                continue;
            } else {
                /* Error - log the problem and don't process the rest of the brigade */
                ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Error reading from bucket");    
                return ap_pass_brigade(f->next, bb);
            }
        }

        APR_BUCKET_REMOVE(bucket);
        APR_BRIGADE_INSERT_TAIL(outBB, new_bucket); 

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
static int amber_should_apply_filter(ap_filter_t *f) {
    return (f && f->r && f->r->content_type && !strcmp("text/html", f->r->content_type));
}

/** 
 * Create the updated bucket to be added to the output filter change
 * @param f the filter
 * @param bucket the bucket to process
 * @param buffer the contents of the bucket
 * @param buffer_size the size of the buffer containing the bucket contents
 * @return the bucket to add to the output filter chain.
 */
static apr_bucket* amber_process_bucket(ap_filter_t *f, apr_bucket *bucket, const char *buffer, size_t buffer_size) {

    ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Buffer contents: %s", buffer);
    amber_matches_t links = find_links_in_bucket(f,bucket);

    return bucket;
}

/** 
 * Search the bucket for links that are candidates to be rewritten, using PCRE
 * @param f the filter
 * @param bucket the bucket being processed
 * @return amber_matches_t containing the number of links found, and arrays with the
 *         link URL and offset within the buffer where any rewriting should occur
 */
static amber_matches_t find_links_in_bucket(ap_filter_t *f, apr_bucket *bucket) {

    amber_matches_t result = { .count = 0 };
    pcre *re;
    const char *pcre_error;
    int pcre_error_offset;

    /* Regex pattern to find urls within hrefs */
    char *pattern = "href=[\"'](http[^\v()<>{}\\[\\]\"']+)['\"]";

    /* Compile the pattern to use */
    if (!(re = pcre_compile(pattern, PCRE_CASELESS, &pcre_error, &pcre_error_offset, NULL))) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "PCRE compilation failed at offset %d: %s", pcre_error_offset, pcre_error);
        return result;
    }

    /* Find out how many subpatterns for matching there are in the pattern */
    int subpattern_captures_count;
    int pcre_result;
    if ((pcre_result = pcre_fullinfo(re, NULL, PCRE_INFO_CAPTURECOUNT, &subpattern_captures_count)) < 0) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "PCRE subpattern capture count failed with code %d", pcre_result);
        return result;
    }

    




// pcre *re;constchar *error;int erroffset;int ovector[OVECCOUNT];int rc, i;if (argc != 3)  {  printf("Two arguments required: a regex and a subject string\n");  return 1;  }/* Compile the regular expression in the first argument */re = pcre_compile(  argv[1],              /* the pattern */  0,                    /* default options */  &error,               /* for error message */  &erroffset,           /* for error offset */  NULL);                /* use default character tables */

    return result;
}

