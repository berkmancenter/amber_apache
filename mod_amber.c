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
    char  **url;        /* Array - urls within the buffer. */
} amber_matches_t;

/* Define prototypes of our functions in this module */
static void register_hooks(apr_pool_t *pool);
static int amber_handler(request_rec *r);
static apr_status_t amber_filter(ap_filter_t *f, apr_bucket_brigade *bb);
static int amber_should_apply_filter(ap_filter_t *f);
static apr_bucket* amber_process_bucket(ap_filter_t *f, apr_bucket *bucket, const char *buffer, size_t buffer_size);
static amber_matches_t find_links_in_buffer(ap_filter_t *f, const char *buffer, size_t buffer_size);
static size_t amber_insert_attributes(ap_filter_t *f, amber_matches_t links, const char *old_buffer, size_t old_buffer_size, const char *new_buffer, size_t new_buffer_size);
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
    
    ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: File type: %s", f->r->content_type);
    return (f && f->r && f->r->content_type && !strncmp("text/html", f->r->content_type, 9));
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

    // ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Buffer contents %d %s", (int)buffer_size, buffer);
    amber_matches_t links = find_links_in_buffer(f, buffer, buffer_size);

    /* If there are no links to evaluate, just return the original bucket */
    if (0 == links.count) {
        return bucket;
    }

    /* If there are links to evaluate, create a new buffer for the updated links. 
       Allocate additional memory for the HTML attributes we are going to add */
    size_t AMBER_ATTRIBUTES_SIZE = 200;
    size_t new_buffer_memory_allocated = buffer_size + (links.count * sizeof(char) * AMBER_ATTRIBUTES_SIZE);
    char *new_buffer = apr_bucket_alloc( new_buffer_memory_allocated, f->c->bucket_alloc);

    // *** UPDATE THE BUFFER HERE *** //
    size_t new_bucket_size = amber_insert_attributes(f, links, buffer, buffer_size, new_buffer, new_buffer_memory_allocated);
    
    /* TODO - CHANGE THIS TO REFLECT THE ACTUAL SIZE OF THE NEW BUCKET */
    apr_bucket *new_bucket = apr_bucket_heap_create(new_buffer, new_bucket_size, NULL, f->c->bucket_alloc);
    return new_bucket;
}

/** 
 * Search a buffer for links that are candidates to be rewritten, using PCRE
 * @param f the filter
 * @param buffer the contents of the bucket
 * @param buffer_size the size of the buffer containing the bucket contents
 * @return amber_matches_t containing the number of links found, and arrays with the
 *         link URL and offset within the buffer where any rewriting should occur
 */
static amber_matches_t find_links_in_buffer(ap_filter_t *f, const char *buffer, size_t buffer_size) {

    amber_matches_t result = { .count = 0, .insert_pos = NULL, .url = NULL };
    int MATCHES_CHUNK_SIZE = 5; /* Allocate memory for this many matches (will increase if required) */
    pcre *re;
    const char *pcre_error;
    int pcre_error_offset;

    /* Regex pattern to find urls within hrefs */
    char *pattern = "href=[\"'](http[^\v()<>{}\\[\\]\"']+)['\"]";

    /* Compile the pattern to use */
    if (!(re = pcre_compile(pattern, PCRE_CASELESS, &pcre_error, &pcre_error_offset, NULL))) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: PCRE compilation failed at offset %d: %s", pcre_error_offset, pcre_error);
        return result;
    }

    /* Find out how many subpatterns for matching there are in the pattern */
    int subpattern_captures_count;
    int pcre_result;
    if ((pcre_result = pcre_fullinfo(re, NULL, PCRE_INFO_CAPTURECOUNT, &subpattern_captures_count)) < 0) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: PCRE subpattern capture count failed with code %d", pcre_result);
        return result;
    }

    /* Setup the structures into which pcre_exec will place the information about any matches */
    int  pcre_results_vector_count = (subpattern_captures_count + 1) * 3;
    int  *pcre_results_vector = apr_palloc(f->r->pool, pcre_results_vector_count * sizeof(int));

    /* Walk through the buffer, until we stop getting matches or get to the end */
    char   *pos = (char *)buffer;
    size_t remaining_buffer = buffer_size;
    
    do {
        pcre_result = pcre_exec(re, NULL, pos, remaining_buffer, 0, 0, pcre_results_vector, pcre_results_vector_count); 
        if (PCRE_ERROR_NOMATCH == pcre_result) { /* No matches */
            break;
        }
        if (pcre_result < 0) {  /* An error occurred */
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Error while matching regular expression %d", pcre_result);
            break;
        }
        if (pcre_result > 0) { /* We have a match! */   
            if (!result.insert_pos || !result.url) {
                /* Lazy allocation of memory for our result structure when it's needed for the first time */
                result.insert_pos = apr_palloc(f->r->pool, MATCHES_CHUNK_SIZE * sizeof(int));
                result.url = apr_palloc(f->r->pool, MATCHES_CHUNK_SIZE * sizeof(char *));
            } else if ((result.count % MATCHES_CHUNK_SIZE) == 0) {
                /* Allocate another chunk of memory if needed */
                int  *new_insert_pos = apr_palloc(f->r->pool, (result.count + MATCHES_CHUNK_SIZE) * sizeof(int));
                char **new_url = apr_palloc(f->r->pool, (result.count + MATCHES_CHUNK_SIZE) * sizeof(char *));
                memcpy(new_insert_pos, result.insert_pos, sizeof(int) * result.count);
                memcpy(new_url, result.url, sizeof(char *) * result.count);
                result.insert_pos = new_insert_pos;
                result.url = new_url;
            }   

            /* pcre_results_vector[0] and pcre_results_vector[1] are for the capture group matching the full regex. 
               pcre_results_vector[2] and pcre_results_vector[3] are for the first capture group within it 
               The start of the full regesx is the insertion point, and the first capture group is the URL */
            result.insert_pos[result.count] = (pos - buffer) + pcre_results_vector[0];
            int url_size = (pcre_results_vector[3] - pcre_results_vector[2] + 1) * sizeof(char);
            result.url[result.count] = apr_palloc(f->r->pool, url_size);
            memcpy(result.url[result.count], pos + pcre_results_vector[2], url_size);
            result.url[result.count][pcre_results_vector[3] - pcre_results_vector[2]] = 0; /* Null-terminate the string */
            result.count++;

            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Match: %s", result.url[result.count-1]);

            pos += pcre_results_vector[1];
            remaining_buffer = buffer - pos + buffer_size;
        }
    } while (pos < (buffer + buffer_size));

    return result;
}

/**
 * Copy data from the old buffer to the new buffer, looking up link attributes and inserting them as we go
 * Links which are not found in the database are enqueued for future caching
 * @param f the filter
 * @param links links detected in the old buffer
 * @param old_buffer buffer with original content 
 * @param old_buffer_size size of old_buffer
 * @param new_buffer buffer which will contain the updated content (memory already allocated)
 * @param new_buffer_size memory allocated for the new buffer 
 * @return actual size of the new buffer
 */
static size_t amber_insert_attributes(ap_filter_t *f, amber_matches_t links, const char *old_buffer, size_t old_buffer_size, const char *new_buffer, size_t new_buffer_size) {

    // Setup database for all the queries we're going to make

    char *src = (char *)old_buffer;
    char *dest = (char *)new_buffer;
    int copy_size;

    for (int i = 0; i < links.count; i++) {  

        /* Copy the data up to the insertion point for the next match */
        copy_size = links.insert_pos[i] + old_buffer - src;
        if (copy_size + dest >= new_buffer + new_buffer_size) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Not enough memory allocated for new buffer");
            // TODO: Is there anything sensible we can do here?
            break;
        }        
        memcpy(dest, src, copy_size);
        src += copy_size;
        dest += copy_size;

        /* Get the attributes to insert, and copy them too */
        char *insert = " data-testing='bananas' ";
        copy_size = strlen(insert);
        // ngx_int_t result = ngx_http_amber_get_attribute(r, sqlite_handle, sqlite_statement, links.url[i], insertion);
        if (copy_size + dest >= new_buffer + new_buffer_size) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Not enough memory allocated for new buffer");
            // TODO: Is there anything sensible we can do here?
            break;
        }
        memcpy(dest, insert, copy_size);
        dest += copy_size;
    }

    /* Copy any remaining content after the last match */
    if (src < old_buffer + old_buffer_size) {
        copy_size = old_buffer + old_buffer_size - src;
        if (dest + copy_size >= new_buffer + new_buffer_size) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server, "Amber: Not enough memory allocated for new buffer");
        } else {
            memcpy(dest, src, copy_size);
            dest += copy_size;
        }
    }

    /* Actual size of the new buffer */
    return dest - new_buffer;
}



