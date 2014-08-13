#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"
#include "apr_strings.h"
#include "http_log.h"
#include "pcre.h"
#include <sqlite3.h>

#define AMBER_ACTION_NONE     0
#define AMBER_ACTION_HOVER    1
#define AMBER_ACTION_POPUP    2
#define AMBER_ACTION_CACHE    3
#define AMBER_STATUS_DOWN     0
#define AMBER_STATUS_UP       1
#define AMBER_MAX_ATTRIBUTE_STRING 250
#define AMBER_CACHE_ATTRIBUTES_ERROR -1
#define AMBER_CACHE_ATTRIBUTES_FOUND 0
#define AMBER_CACHE_ATTRIBUTES_EMPTY 1
#define AMBER_CACHE_ATTRIBUTES_NOT_FOUND 2

/* Macros for debug and error logging */
#define amber_debug(mess) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, f->r->server, mess)
#define amber_debug1(mess,p1) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, f->r->server, mess, p1)
#define amber_debug2(mess,p1,p2) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, f->r->server, mess, p1, p2)
#define amber_debug4(mess,p1,p2,p3,p4) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_DEBUG, 0, f->r->server, mess, p1, p2, p3, p4)
#define amber_error(mess) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, f->r->server, mess)
#define amber_error1(mess,p1) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, f->r->server, mess, p1)
#define amber_error2(mess,p1,p2) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_EMERG, 0, f->r->server, mess, p1, p2)

/* Structure representing the complete list of URLs found within a chunk of HTML, with offsets */
typedef struct {
    int   count;        /* Number of matching insertion positions and urls */
    int   *insert_pos;  /* Array - positions within the buffer where additional 
                           attributes should in inserted for matching hrefs */
    char  **url;        /* Array - urls within the buffer. */
} amber_matches_t;

/* Configuration settings */
typedef struct {
    int        enabled;                  /* Is Amber enabled? */
    char *     database;                 /* Path to the sqlite database */
    int        behavior_up;              /* Default behaviour when site is up */
    int        behavior_down;            /* Default behaviour when site is down */
    int        hover_delay_up;           /* Hover delay when site is up */
    int        hover_delay_down;         /* Hover delay when site is down */
    char *     country;                  /* Two-character country code for country-specific behavior */
    int        country_behavior_up;      /* Default behaviour when site is up */
    int        country_behavior_down;    /* Default behaviour when site is down */
    int        country_hover_delay_up;   /* Hover delay when site is up */
    int        country_hover_delay_down; /* Hover delay when site is down */
    int        cache_delivery;          
} amber_options_t;

typedef struct {
    int        activity_logged;
} amber_context_t;

/* Functions and callbacks specifically related to Apache integration */
static void         register_hooks(apr_pool_t *pool);
static void*        amber_create_dir_conf(apr_pool_t* pool, char* x);
static void*        amber_merge_dir_conf(apr_pool_t* pool, void* BASE, void* ADD);
static const char*  amber_set_behavior_up(cmd_parms *cmd, void *cfg, const char *arg);
static const char*  amber_set_behavior_down(cmd_parms *cmd, void *cfg, const char *arg);
static const char*  amber_set_country_behavior_up(cmd_parms *cmd, void *cfg, const char *arg);
static const char*  amber_set_country_behavior_down(cmd_parms *cmd, void *cfg, const char *arg);
static apr_status_t amber_filter(ap_filter_t *f, apr_bucket_brigade *bb);

/* Other functions */
static int              amber_should_apply_filter(ap_filter_t *f);
static int              amber_is_cache_delivery(ap_filter_t *f);
static apr_bucket*      amber_process_bucket(ap_filter_t *f, apr_bucket *bucket, const char *buffer, size_t buffer_size);
static amber_matches_t  find_links_in_buffer(ap_filter_t *f, const char *buffer, size_t buffer_size);
static size_t           amber_insert_attributes(ap_filter_t *f, amber_matches_t links, const char *old_buffer, size_t old_buffer_size, const char *new_buffer, size_t new_buffer_size);
static char*            get_cache_item_id(ap_filter_t *f);
static int              amber_log_activity(ap_filter_t *f);
static int              amber_set_cache_content_type(ap_filter_t *f);

/* Functions that interact with the database */
static sqlite3*         amber_db_get_database(ap_filter_t *f, char *db_path);
static int              amber_db_finalize_statement (ap_filter_t *f, sqlite3_stmt *sqlite_statement);
static int              amber_db_close_database(ap_filter_t *f, sqlite3 *sqlite_handle);
static sqlite3_stmt*    amber_db_get_statement(ap_filter_t *f, sqlite3 *sqlite_handle, char *statement);

static sqlite3_stmt*    amber_db_get_url_lookup_query(ap_filter_t *f, sqlite3 *sqlite_handle);
static sqlite3_stmt*    amber_db_get_enqueue_url_query(ap_filter_t *f, sqlite3 *sqlite_handle);
static sqlite3_stmt*    amber_db_get_log_activity_query(ap_filter_t *f, sqlite3 *sqlite_handle);
static sqlite3_stmt*    amber_db_get_content_type_query(ap_filter_t *f, sqlite3 *sqlite_handle);
static int              amber_db_enqueue_url(ap_filter_t *f, sqlite3 *sqlite_handle, char *url);
static int              amber_db_get_attribute(ap_filter_t *f, sqlite3 *sqlite_handle, sqlite3_stmt *sqlite_statement, char * url, char **result);

/* Utility functions (platform independent) */
int amber_get_behavior(amber_options_t *options, unsigned char *out, int status);
int amber_build_attribute(amber_options_t *options, unsigned char *out, char *location, int status, time_t date);

/* Apache structure that defines how configuration settings should be handled */
static const command_rec amber_directives[] =
{
    AP_INIT_FLAG("AmberEnabled",                ap_set_flag_slot, (void*)APR_OFFSETOF(amber_options_t, enabled), ACCESS_CONF, "Enable Amber"),
    AP_INIT_TAKE1("AmberDatabase",              ap_set_file_slot, (void*)APR_OFFSETOF(amber_options_t, database), ACCESS_CONF, "Location of the Amber database"),
    AP_INIT_TAKE1("AmberBehaviorUp",            amber_set_behavior_up, NULL, ACCESS_CONF, "Set the behavior for links that are available"),
    AP_INIT_TAKE1("AmberBehaviorDown",          amber_set_behavior_down, NULL, ACCESS_CONF, "Set the behavior for links that are not available"),
    AP_INIT_TAKE1("AmberHoverDelayUp",          ap_set_int_slot, (void*)APR_OFFSETOF(amber_options_t, hover_delay_up), ACCESS_CONF, "Set the hover delay for links that are available"),
    AP_INIT_TAKE1("AmberHoverDelayDown",        ap_set_int_slot, (void*)APR_OFFSETOF(amber_options_t, hover_delay_down), ACCESS_CONF, "Set the hover delay for links that are not available"),
    AP_INIT_TAKE1("AmberCountry",               ap_set_string_slot, (void*)APR_OFFSETOF(amber_options_t, country), ACCESS_CONF, "Set the behavior for users from a particular country"),
    AP_INIT_TAKE1("AmberCountryBehaviorUp",     amber_set_country_behavior_up, NULL, ACCESS_CONF, "Set the behavior for links that are available in the specified country"),
    AP_INIT_TAKE1("AmberCountryBehaviorDown",   amber_set_country_behavior_down, NULL, ACCESS_CONF, "Set the behavior for links that are not available in the specified country"),
    AP_INIT_TAKE1("AmberCountryHoverDelayUp",   ap_set_int_slot, (void*)APR_OFFSETOF(amber_options_t, country_hover_delay_up), ACCESS_CONF, "Set the hover delay for links that are available for the specified country"),
    AP_INIT_TAKE1("AmberCountryHoverDelayDown", ap_set_int_slot, (void*)APR_OFFSETOF(amber_options_t, country_hover_delay_down), ACCESS_CONF, "Set the hover delay for links that are not available for the specified country"),
    AP_INIT_FLAG("AmberCacheDelivery",          ap_set_flag_slot, (void*)APR_OFFSETOF(amber_options_t, cache_delivery), ACCESS_CONF, "Enable for directory from which cached content will be served "),
    { NULL }
};

/* Main apache structure that defines this as an Apache module */
module AP_MODULE_DECLARE_DATA   amber_module =
{
    STANDARD20_MODULE_STUFF,
    amber_create_dir_conf,            // Per-directory configuration handler
    amber_merge_dir_conf,             // Merge handler for per-directory configurations
    NULL,                             // Per-server configuration handler
    NULL,                             // Merge handler for per-server configurations
    amber_directives,                 // Any directives we may have for httpd
    register_hooks                    // Our hook registering function
};

/* Apache: Adds a hook to the httpd process */
static void register_hooks(apr_pool_t *pool) 
{
    ap_register_output_filter("amber-filter", amber_filter, NULL, AP_FTYPE_RESOURCE) ;
}

/**
 * Apache: Set the default values for the configuration directives. 
 */
static void* amber_create_dir_conf(apr_pool_t* pool, char* x) {
    amber_options_t* options = apr_pcalloc(pool, sizeof(amber_options_t));
  
    if (options) {
        options->enabled = -1;
        options->database = NULL;
        options->behavior_up = -1;
        options->behavior_down = -1;
        options->hover_delay_up = -1;
        options->hover_delay_down = -1;
        options->country = NULL;
        options->country_behavior_up = -1;
        options->country_behavior_down = -1;
        options->country_hover_delay_up = -1;
        options->country_hover_delay_down = -1;
        options->cache_delivery = -1;
    }
    return options ;
}

/**
 * Apache: Merge hierarchical configuration settings 
 */
static void* amber_merge_dir_conf(apr_pool_t* pool, void* BASE, void* ADD) {
    amber_options_t* base = BASE ;
    amber_options_t* add = ADD ;
    amber_options_t* conf = apr_palloc(pool, sizeof(amber_options_t)) ;

    conf->enabled                   =  ( add->enabled == -1 ) ? base->enabled : add->enabled ;
    conf->database                  =  ( !add->database ) ? base->database : add->database ;
    conf->behavior_up               =  ( add->behavior_up == -1 ) ? base->behavior_up : add->behavior_up ;
    conf->behavior_down             =  ( add->behavior_down == -1 ) ? base->behavior_down : add->behavior_down ;
    conf->hover_delay_up            =  ( add->hover_delay_up == -1 ) ? base->hover_delay_up : add->hover_delay_up ;
    conf->hover_delay_down          =  ( add->hover_delay_down == -1 ) ? base->hover_delay_down : add->hover_delay_down ;
    conf->country                   =  ( !add->country ) ? base->country : add->country ;
    conf->country_behavior_up       =  ( add->country_behavior_up == -1 ) ? base->country_behavior_up : add->country_behavior_up ;
    conf->country_behavior_down     =  ( add->country_behavior_down == -1 ) ? base->country_behavior_down : add->country_behavior_down ;
    conf->country_hover_delay_up    =  ( add->country_hover_delay_up == -1 ) ? base->country_hover_delay_up : add->country_hover_delay_up ;
    conf->country_hover_delay_down  =  ( add->country_hover_delay_down == -1 ) ? base->country_hover_delay_down : add->country_hover_delay_down ;
    conf->cache_delivery            =  ( add->cache_delivery == -1 ) ? base->cache_delivery : add->cache_delivery ;
    return conf ;
}

/** 
 * Convert strings from a configuration file describing behavior into integers
 * @param s description of behavior
 * @return integer corresponding to one of out defined actions for cached content
 */
static int amber_convert_behavior_config(const char *s) {
    if (!strcmp("cache",s)) {
        return AMBER_ACTION_CACHE;
    } else if (!strcmp("popup",s)) {
        return AMBER_ACTION_POPUP;
    } else if (!strcmp("hover",s)) {
        return AMBER_ACTION_HOVER;
    } else {
        return AMBER_ACTION_NONE;
    }
}

/* Callback functions for setting up some configuration settings */
static const char *amber_set_behavior_up(cmd_parms *cmd, void *cfg, const char *arg)
{
    ((amber_options_t*)cfg)->behavior_up = amber_convert_behavior_config(arg);
    return NULL;
}

static const char *amber_set_behavior_down(cmd_parms *cmd, void *cfg, const char *arg)
{
    ((amber_options_t*)cfg)->behavior_down = amber_convert_behavior_config(arg);
    return NULL;
}

static const char *amber_set_country_behavior_up(cmd_parms *cmd, void *cfg, const char *arg)
{
    ((amber_options_t*)cfg)->country_behavior_up = amber_convert_behavior_config(arg);
    return NULL;
}

static const char *amber_set_country_behavior_down(cmd_parms *cmd, void *cfg, const char *arg)
{
    ((amber_options_t*)cfg)->country_behavior_down = amber_convert_behavior_config(arg);
    return NULL;
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
    apr_status_t        rv;
    amber_context_t     *context;

    amber_debug("Filter start");
    context = f->ctx;
    if (!context) {
        f->ctx = context = apr_palloc(f->r->pool, sizeof(amber_context_t));
    }

    if (amber_is_cache_delivery(f)) {
        amber_debug("Delivering cached item");
        if (!context->activity_logged) {
            amber_log_activity(f);
            amber_set_cache_content_type(f);
        }
        return ap_pass_brigade(f->next, bb);
    }

    if (!amber_should_apply_filter(f)) {
        amber_debug("Skipping file");
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
        amber_error("Amber: Could not create output buffer brigade");    
        return ap_pass_brigade(f->next, bb);
    } 


    /* Start out with non-blocking reads, so that if we're reading from a stream, we don't 
       unecessarily block until the stream is complete */     
    apr_read_type_e read_mode = APR_NONBLOCK_READ; 
    bucket = APR_BRIGADE_FIRST(bb);

    /* Main loop through which we process all the buckets in the brigade */
    while (bucket != APR_BRIGADE_SENTINEL(bb)) {
        amber_debug("In bucket loop");
        next_bucket = APR_BUCKET_NEXT(bucket);
        if (bucket == next_bucket) {
            amber_error("Avoiding infinite bucket loop!");
            return ap_pass_brigade(f->next, bb);
        }

        /* This is a metadata bucket indicating the end of the brigade */
        if (APR_BUCKET_IS_EOS(bucket)) {
            APR_BUCKET_REMOVE(bucket);
            APR_BRIGADE_INSERT_TAIL(outBB, bucket);
            amber_debug("Filter end");
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
                amber_error("Error reading from bucket");    
                return ap_pass_brigade(f->next, bb);
            }
        }

        APR_BUCKET_REMOVE(bucket);
        APR_BRIGADE_INSERT_TAIL(outBB, new_bucket); 

        bucket = next_bucket;
    }

    amber_debug("Filter end");

    return ap_pass_brigade(f->next, outBB);
}    

/**
 * Determine whether or not our filter should process this request for link rewriting. 
 * We only want to process HTML files where Amber is enabled
 * @param f the filter
 * @return true if the requests should be processed
 */
static int amber_should_apply_filter(ap_filter_t *f) {
    
    amber_debug1("Amber: File type: %s", f->r->content_type);
    amber_options_t *options = (amber_options_t*) ap_get_module_config(f->r->per_dir_config, &amber_module); 
    return (
        f && 
        f->r && 
        f->r->content_type && 
        (1 == options->enabled) && 
        !strncmp("text/html", f->r->content_type, 9));
}

/**
 * Determine whether this requests is serving cached content (not assets, but the primary content)
 * If so, we'll want to take some action (logging activity, perhaps setting the content-type)
 * @param f the filter
 * @return true if this is a request for cached content 
 */
static int amber_is_cache_delivery(ap_filter_t *f) {    
    amber_options_t *options = (amber_options_t*) ap_get_module_config(f->r->per_dir_config, &amber_module); 
    return (options->cache_delivery == 1);
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

    size_t new_bucket_size = amber_insert_attributes(f, links, buffer, buffer_size, new_buffer, new_buffer_memory_allocated);
    if (0 == new_bucket_size) {
        return bucket; /* An error, so just return the old bucket */
    }
    
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
        amber_error2("Amber: PCRE compilation failed at offset %d: %s", pcre_error_offset, pcre_error);
        return result;
    }

    /* Find out how many subpatterns for matching there are in the pattern */
    int subpattern_captures_count;
    int pcre_result;
    if ((pcre_result = pcre_fullinfo(re, NULL, PCRE_INFO_CAPTURECOUNT, &subpattern_captures_count)) < 0) {
        amber_error1("Amber: PCRE subpattern capture count failed with code %d", pcre_result);
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
            amber_error1("Amber: Error while matching regular expression %d", pcre_result);
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

            amber_debug1("Amber: Match: %s", result.url[result.count-1]);

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
 * @return actual size of the new buffer (or 0 on error)
 */
static size_t amber_insert_attributes(ap_filter_t *f, amber_matches_t links, const char *old_buffer, size_t old_buffer_size, const char *new_buffer, size_t new_buffer_size) {

    // Setup database for all the queries we're going to make

    char *src = (char *)old_buffer;
    char *dest = (char *)new_buffer;
    int copy_size;

    amber_options_t *options = (amber_options_t*) ap_get_module_config(f->r->per_dir_config, &amber_module); 
    sqlite3 *sqlite_handle = amber_db_get_database(f, options->database);
    if (!sqlite_handle) {
        return 0;
    }

    sqlite3_stmt *sqlite_statement = amber_db_get_url_lookup_query(f,sqlite_handle);
    if (!sqlite_statement) {
        return 0;
    }

    for (int i = 0; i < links.count; i++) {  

        /* Copy the data up to the insertion point for the next match */
        copy_size = links.insert_pos[i] + old_buffer - src;
        if (copy_size + dest >= new_buffer + new_buffer_size) {
            amber_error("Amber: Not enough memory allocated for new buffer");
            // TODO: Is there anything sensible we can do here?
            break;
        }        
        memcpy(dest, src, copy_size);
        src += copy_size;
        dest += copy_size;

        /* Get the attributes to insert, and copy them too */
        char *insert;
        int result = amber_db_get_attribute(f, sqlite_handle, sqlite_statement, links.url[i], &insert);

        if (AMBER_CACHE_ATTRIBUTES_NOT_FOUND == result) {
            /* If the URL is not found, queue it up to be cached later */
            amber_db_enqueue_url(f, sqlite_handle, links.url[i]);
        } else if (AMBER_CACHE_ATTRIBUTES_FOUND == result) {
            /* If the URL is found, insert the attributes we got */
            copy_size = strlen(insert);
            if (copy_size + dest >= new_buffer + new_buffer_size) {
                amber_error("Amber: Not enough memory allocated for new buffer");
                // TODO: Is there anything sensible we can do here?
                break;
            }
            memcpy(dest, insert, copy_size);
            dest += copy_size;
        }
    }

    amber_db_finalize_statement(f, sqlite_statement);
    amber_db_close_database(f, sqlite_handle);

    /* Copy any remaining content after the last match */
    if (src < old_buffer + old_buffer_size) {
        copy_size = old_buffer + old_buffer_size - src;
        if (dest + copy_size >= new_buffer + new_buffer_size) {
            amber_error("Amber: Not enough memory allocated for new buffer");
        } else {
            memcpy(dest, src, copy_size);
            dest += copy_size;
        }
    }
    /* Actual size of the new buffer */
    return dest - new_buffer;
}

/**
 * Get the cache id of an item being served from the cache in the current request
 * @param f the filter
 * @return pointer to cache id, or null if not found
 */
static char* get_cache_item_id(ap_filter_t *f) {

    char *uri = apr_pstrdup(f->r->pool, f->r->uri);

    if (!uri) {
        return NULL;
    }

    /* Trim any trailing '/' */
    if ((strlen(uri) > 0) && (uri[strlen(uri) - 1] == '/')) {
        uri[strlen(uri) - 1] = 0;
    }

    char *pos = strrchr(uri, '/');
    if (pos && pos[1]) {
        pos++;
        return pos;
    } else {
        return NULL;
    }
}

/**
 * Log a view of a cached item to the amber_activity table
 * @param f the filter
 * @return 0 on success
 */
static int amber_log_activity(ap_filter_t *f) {
    int sqlite_rc;
    char *cache_id = get_cache_item_id(f);

    if (cache_id) {
        amber_debug1("Logging activity for cache item: [%s]", cache_id);

        amber_options_t *options = (amber_options_t*) ap_get_module_config(f->r->per_dir_config, &amber_module); 
        sqlite3 *sqlite_handle = amber_db_get_database(f, options->database);
        if (!sqlite_handle) {
            return -1;
        }

        sqlite3_stmt *sqlite_statement = amber_db_get_log_activity_query(f,sqlite_handle);
        if (!sqlite_statement) {
            return -1;
        }

        if ((sqlite_rc = sqlite3_bind_text(sqlite_statement, 1, cache_id, strlen(cache_id), SQLITE_STATIC)) != SQLITE_OK) {
            amber_error2("Amber: error binding sqlite parameter: %s (%d)", cache_id, sqlite_rc);
            amber_db_finalize_statement(f ,sqlite_statement);
            return -1;
        }

        sqlite_rc = sqlite3_bind_int(sqlite_statement, 2, time(NULL));
        if (sqlite_rc != SQLITE_OK) {
            amber_error2("Amber: error binding sqlite parameter: %s (%d)", "time()", sqlite_rc);
            amber_db_finalize_statement(f ,sqlite_statement);
            amber_db_close_database(f, sqlite_handle);
            return -1;
        }

        sqlite_rc = sqlite3_step(sqlite_statement);
        if (sqlite_rc == SQLITE_DONE) { /* No data returned */
            amber_debug1("Logged cache visit: %s", cache_id);
        } else {
            amber_debug2("Error logging cache visit: %s (%d)", cache_id, sqlite_rc);
            amber_error("Amber: error writing sqlite database. Make sure database file and its directory are writable");
        }
        amber_db_finalize_statement(f, sqlite_statement);
        amber_db_close_database(f, sqlite_handle);
        return 0;

    }
    return 0;
}

/**
 * Set the content-type of a cached item that we're returning from the cache
 * @param f the filter
 * @return 0 on success
 */
static int amber_set_cache_content_type(ap_filter_t *f) {
    int sqlite_rc;
    char *cache_id = get_cache_item_id(f);

    if (cache_id) {
        amber_debug1("Setting content type for cache item: [%s]", cache_id);

        amber_options_t *options = (amber_options_t*) ap_get_module_config(f->r->per_dir_config, &amber_module); 
        sqlite3 *sqlite_handle = amber_db_get_database(f, options->database);
        if (!sqlite_handle) {
            return -1;
        }

        sqlite3_stmt *sqlite_statement = amber_db_get_content_type_query(f,sqlite_handle);
        if (!sqlite_statement) {
            return -1;
        }

        if ((sqlite_rc = sqlite3_bind_text(sqlite_statement, 1, cache_id, strlen(cache_id), SQLITE_STATIC)) != SQLITE_OK) {
            amber_error2("Amber: error binding sqlite parameter: %s (%d)", cache_id, sqlite_rc);
            amber_db_finalize_statement(f ,sqlite_statement);
            amber_db_close_database(f, sqlite_handle);
            return -1;
        }

        sqlite_rc = sqlite3_step(sqlite_statement);
        if (sqlite_rc == SQLITE_DONE) { /* No data returned */
            amber_debug1("No content type found when serving cache item: %s", cache_id); 
        } else if (sqlite_rc == SQLITE_ROW) {
            char *mimetype = (char *)sqlite3_column_text(sqlite_statement,0);
            strncpy((char *)f->r->content_type, mimetype, strlen(mimetype));
            amber_debug2("Set content type for cache item (%s): %s", cache_id, mimetype);

        } else {
            amber_error2("Error retrieving cache item content type: %s (%d)", cache_id, sqlite_rc);
            amber_db_finalize_statement(f, sqlite_statement);
            amber_db_close_database(f, sqlite_handle);
            return -1;
        }
        amber_db_finalize_statement(f, sqlite_statement);
        amber_db_close_database(f, sqlite_handle);
    }
    return 0;
}

/* ======================================================================== */
/* Database access code - could be moved to a separate file                 */
/* ======================================================================== */

/** 
 * Get a handle to the sqlite database
 * @param f the filter
 * @param db_path location of the sqlite database on disk
 * @return handle to the open sqlite database. If failed to open, return null
 */ 
static sqlite3 *amber_db_get_database(ap_filter_t *f, char *db_path) {
    sqlite3 *sqlite_handle;
    int sqlite_rc;

    amber_debug1("Database: %s", db_path);
    sqlite_rc = sqlite3_open(db_path, &sqlite_handle);
    if (sqlite_rc) {
        /* GCC thinks these are not being used for some reason, so annotate to avoid compiler warnings */
        /* TODO: See if we can remove these */
        __attribute__ ((__unused__)) int extended_rc = sqlite3_extended_errcode(sqlite_handle);
        __attribute__ ((__unused__)) const char * msg = sqlite3_errmsg(sqlite_handle);
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, f->r->server,
            "Amber: Error opening sqlite database (%d,%s). Make sure database file and its directory are writable", sqlite_rc, db_path);
        sqlite3_close(sqlite_handle);
        return NULL;
    }
    return sqlite_handle;
}

static int amber_db_finalize_statement (ap_filter_t *f, sqlite3_stmt *sqlite_statement) {
    int sqlite_rc;
    if ((sqlite_rc = sqlite3_finalize(sqlite_statement)) != SQLITE_OK) {
        amber_error1("AMBER error finalizing statement (%d)", sqlite_rc);
    }
    return sqlite_rc;
}

/**
 * Close the database
 * @param f the filter
 * @param sqlite_handle handle to the database to close
 * @return sqlite3 status code
 */
static int amber_db_close_database(ap_filter_t *f, sqlite3 *sqlite_handle) {
    int sqlite_rc;
    if ((sqlite_rc = sqlite3_close(sqlite_handle)) != SQLITE_OK) {
        amber_error1("Amber: error closing sqlite database (%d)", sqlite_rc);
    }
    return sqlite_rc;
}

/**
 * Prepare a sql query
 * @param f the filter
 * @param sqlite_handle handle to the database to use
 * @param statement SQL query with '?' for variable parameters
 * @return sqlite_statement to be executed
 */
static sqlite3_stmt *amber_db_get_statement(ap_filter_t *f, sqlite3 *sqlite_handle, char *statement) {
    sqlite3_stmt *sqlite_statement;
    const char *query_tail;
    int sqlite_rc = sqlite3_prepare_v2(sqlite_handle, statement, -1, &sqlite_statement, &query_tail);
    if (sqlite_rc != SQLITE_OK) {
        amber_error1("AMBER error creating sqlite prepared statement (%d)", sqlite_rc);
        amber_db_close_database(f, sqlite_handle);
        return NULL;
    }
    return sqlite_statement;
}

/**
 * Prepare a sql query for retrieving information about a url
 * @param f the filter
 * @param sqlite_handle handle to the database to use
 * @return sqlite_statement to be executed
 */
static sqlite3_stmt *amber_db_get_url_lookup_query(ap_filter_t *f, sqlite3 *sqlite_handle) {
    return amber_db_get_statement(f, sqlite_handle, "SELECT aa.location, aa.date, ah.status FROM amber_cache aa, amber_check ah WHERE aa.url = ? AND aa.id = ah.id");
}

/**
 * Prepare a sql query for enqueuing url
 * @param f the filter
 * @param sqlite_handle handle to the database to use
 * @return sqlite_statement to be executed
 */
static sqlite3_stmt *amber_db_get_enqueue_url_query(ap_filter_t *f, sqlite3 *sqlite_handle) {
    return amber_db_get_statement(f, sqlite_handle, "INSERT OR IGNORE INTO amber_queue (url, created) SELECT ?1,?2 where ?1 not in (select url from amber_exclude) and ?1 not in (select url from amber_check)");
}

/**
 * Prepare a sql query for logging activity
 * @param f the filter
 * @param sqlite_handle handle to the database to use
 * @return sqlite_statement to be executed
 */
static sqlite3_stmt *amber_db_get_log_activity_query(ap_filter_t *f, sqlite3 *sqlite_handle) {
    return amber_db_get_statement(f, sqlite_handle, "INSERT OR REPLACE INTO amber_activity (id, date, views) VALUES (?1, ?2, COALESCE ((SELECT views+1 from amber_activity where id = ?1), 1))");
}

/**
 * Prepare a sql query for getting the mime-type of a cached item
 * @param f the filter
 * @param sqlite_handle handle to the database to use
 * @return sqlite_statement to be executed
 */
static sqlite3_stmt *amber_db_get_content_type_query(ap_filter_t *f, sqlite3 *sqlite_handle) {
    return amber_db_get_statement(f, sqlite_handle, "SELECT type FROM amber_cache WHERE id = ?");
}

/**
 * Get the AMBER attributes that should be added to the HREF with the given target URL, based on data from the cache.
 * @param f the filter
 * @param sqlite_handle handle to the database to use
 * @param sqlite_statement prepared statement to use in the query
 * @param url to lookup
 * @param result pointer to the attributes to be added (if any)
 * @return status code indicating the results of the query:
 *      AMBER_CACHE_ATTRIBUTES_ERROR - there was an error
 *      AMBER_CACHE_ATTRIBUTES_FOUND - the URL was found and attributes to insert in the HREF are in the result parameter
 *      AMBER_CACHE_ATTRIBUTES_EMPTY -  the URL was found, but there is no cache 
 *      AMBER_CACHE_ATTRIBUTES_NOT_FOUND - the URL was not found
 */
static int amber_db_get_attribute(ap_filter_t *f, sqlite3 *sqlite_handle, sqlite3_stmt *sqlite_statement, char * url, char **result) {

    int rc;
    const char *location_tmp;
    char *location;
    int date;
    int status;

    /* Clear any existing bindings on the statement, in case it had been used before. sqlite3_reset does not do this */
    if ((rc = sqlite3_clear_bindings(sqlite_statement)) != SQLITE_OK) {
        amber_error2("Amber: error error clearing bindings: %s (%d)", url, rc);
        return AMBER_CACHE_ATTRIBUTES_ERROR;
    }

    /* Reset the query to be executed again */
    if ((rc = sqlite3_reset(sqlite_statement)) != SQLITE_OK) {
        amber_error2("Amber: error reseting prepared statement: %s (%d)", url, rc);
        return AMBER_CACHE_ATTRIBUTES_ERROR;
    }

    /* Bind parameter 1 - the URL to lookup */
    if ((rc = sqlite3_bind_text(sqlite_statement, 1, url, strlen(url), SQLITE_STATIC)) != SQLITE_OK) {
        amber_error2("Amber: error binding sqlite parameter: %s (%d)", url, rc);
        return AMBER_CACHE_ATTRIBUTES_ERROR;
    }

    /* Get the first result (the only one we care about) */
    rc = sqlite3_step(sqlite_statement);
    if (rc == SQLITE_DONE) {                 /* No data returned */
        return AMBER_CACHE_ATTRIBUTES_NOT_FOUND;
    } else if (rc == SQLITE_ROW) {           /* Some data found - extract the results */
        /* Copy the location string, since it gets clobbered when the sqlite objects are closed */
        location = apr_pstrdup(f->r->pool, (const char *) sqlite3_column_text(sqlite_statement, 0));
        date = sqlite3_column_int(sqlite_statement,1);
        status = sqlite3_column_int(sqlite_statement,2);
        amber_debug4("Amber: sqlite results for url: (%s) %s, %d, %d", url, location, date, status);
    } else {
        amber_error1("Amber: error executing sqlite statement: (%d)", rc);
        return AMBER_CACHE_ATTRIBUTES_ERROR;
    }

    /* If the location is empty, no cache exists */
    if (strlen(location) == 0) {
        return AMBER_CACHE_ATTRIBUTES_EMPTY;
    } else {
        amber_options_t *options = (amber_options_t*) ap_get_module_config(f->r->per_dir_config, &amber_module); 

        char *attribute = apr_pcalloc(f->r->pool, AMBER_MAX_ATTRIBUTE_STRING * sizeof(char));
        if ((rc = amber_build_attribute(options, (unsigned char *)attribute, location, status, date))) {
            amber_error1("Amber: error generating attribute string (%d)", rc);
            return AMBER_CACHE_ATTRIBUTES_ERROR;
        }
        amber_debug2("Amber: attribute string for url: (%s) : %s", url, attribute);
        *result = attribute;
    }

    return AMBER_CACHE_ATTRIBUTES_FOUND;
}

/**
 * Add the URL to the amber_queue table so that it will be cached during the next caching run
 * @param f the filter
 * @param sqlite_handle handle to the database to use
 * @param url to enqueue
 * @return 0 on success
*/
static int amber_db_enqueue_url(ap_filter_t *f, sqlite3 *sqlite_handle, char *url) {
    int sqlite_rc;

    sqlite3_stmt *sqlite_statement = amber_db_get_enqueue_url_query(f, sqlite_handle);
    if (!sqlite_statement) {
        return -1;
     }

    if ((sqlite_rc = sqlite3_bind_text(sqlite_statement, 1, url, strlen(url), SQLITE_STATIC)) != SQLITE_OK) {
        amber_error2("Amber: error binding sqlite parameter: %s (%d)", url, sqlite_rc);
        amber_db_finalize_statement(f ,sqlite_statement);
        return -1;
    }
    sqlite_rc = sqlite3_bind_int(sqlite_statement, 2, time(NULL));
    if (sqlite_rc != SQLITE_OK) {
        amber_error2("Amber: error binding sqlite parameter: %s (%d)", "time()", sqlite_rc);
        amber_db_finalize_statement(f ,sqlite_statement);
        return -1;
    }
    sqlite_rc = sqlite3_step(sqlite_statement);
    if (sqlite_rc == SQLITE_DONE) { /* No data returned */
        amber_debug1("Enqueued URL: %s", url);
    } else {
        amber_debug2("Error enqueuing URL: %s (%d)", url, sqlite_rc);
        amber_error("Amber: error writing sqlite database. Make sure database file and its directory are writable");
    }
    amber_db_finalize_statement(f, sqlite_statement);
    return 0;
}


/* ======================================================================== */
/* Amber Utilities - from amber_utils.c in robustness_nginx                 */   
/* Platform-independent and could be moved to a separate file               */
/* ======================================================================== */

#define AMBER_MAX_BEHAVIOR_STRING 20
#define AMBER_MAX_DATE_STRING 30

/* Create a string containing attributes to be added to the HREF

    amber_options_t *options : configuration settings
    char *out               : buffer to where the attribute is written
    chatr *locatino         : location of the cached copy
    int status              : whether the site is up or down
    time_t date             : when the cache was generated (unix epoch)

    returns 0 on success
*/
int amber_build_attribute(amber_options_t *options, unsigned char *out, char *location, int status, time_t date)
{
    unsigned char behavior[AMBER_MAX_BEHAVIOR_STRING];
    char date_string[AMBER_MAX_DATE_STRING];

    int rc = amber_get_behavior(options, behavior, status);
    if (!rc && (strlen((char*)behavior) > 0)) {
        struct tm *timeinfo = localtime(&date);
        strftime(date_string,AMBER_MAX_DATE_STRING,"%FT%T%z",timeinfo);
        snprintf((char *)out,
             AMBER_MAX_ATTRIBUTE_STRING,
             "data-cache='/%s %s' data-amber-behavior='%s' ",
             location,
             date_string,
             behavior
             );
         }
    return rc;
}

/* Get the contents of behavior attribute based on the status of the link
   and the configuration settings.

   amber_options_t *options : configuration settings
   char *out               : buffer to where the attribute is written
   int status              : whether the site is up or down

   returns 0 on success

   TODO: Country-specific behavior
   */
int amber_get_behavior(amber_options_t *options, unsigned char *out, int status) {
    if (!options || !out) {
        return 1;
    }
    if (status == AMBER_STATUS_UP) {
        switch (options->behavior_up) {
            case AMBER_ACTION_HOVER:
                snprintf((char *)out,
                         AMBER_MAX_ATTRIBUTE_STRING,
                         "up hover:%d",
                         options->hover_delay_up);
                break;
            case AMBER_ACTION_POPUP:
                snprintf((char *)out, AMBER_MAX_ATTRIBUTE_STRING, "up popup");
                break;
            case AMBER_ACTION_CACHE:
                snprintf((char *)out, AMBER_MAX_ATTRIBUTE_STRING, "up cache");
                break;
            case AMBER_ACTION_NONE:
                out[0] = 0;
                break;
        }
    } else if (status == AMBER_STATUS_DOWN) {
        switch (options->behavior_down) {
            case AMBER_ACTION_HOVER:
                snprintf((char *)out,
                         AMBER_MAX_ATTRIBUTE_STRING,
                         "down hover:%d",
                         options->hover_delay_down);
                break;
            case AMBER_ACTION_POPUP:
                snprintf((char *)out, AMBER_MAX_ATTRIBUTE_STRING, "down popup");
                break;
            case AMBER_ACTION_CACHE:
                snprintf((char *)out, AMBER_MAX_ATTRIBUTE_STRING, "down cache");
                break;
            case AMBER_ACTION_NONE:
                out[0] = 0;
                break;

        }
    }
    if (strlen(options->country)) {
        char country_attribute[AMBER_MAX_ATTRIBUTE_STRING];
        if (status == AMBER_STATUS_UP) {
            switch (options->country_behavior_up) {
                case AMBER_ACTION_HOVER:
                    snprintf(country_attribute,
                             AMBER_MAX_ATTRIBUTE_STRING,
                             ",%s up hover:%d",
                             options->country,
                             options->country_hover_delay_up);
                    break;
                case AMBER_ACTION_POPUP:
                    snprintf(country_attribute, AMBER_MAX_ATTRIBUTE_STRING, ",%s up popup", options->country);
                    break;
                case AMBER_ACTION_CACHE:
                    snprintf(country_attribute, AMBER_MAX_ATTRIBUTE_STRING, ",%s up cache", options->country);
                    break;
                case AMBER_ACTION_NONE:
                    break;
            }
        } else if (status == AMBER_STATUS_DOWN) {
            switch (options->country_behavior_down) {
                case AMBER_ACTION_HOVER:
                    snprintf(country_attribute,
                             AMBER_MAX_ATTRIBUTE_STRING,
                             ",%s down hover:%d",
                             options->country,
                             options->country_hover_delay_down);
                    break;
                case AMBER_ACTION_POPUP:
                    snprintf(country_attribute, AMBER_MAX_ATTRIBUTE_STRING, ",%s down popup", options->country);
                    break;
                case AMBER_ACTION_CACHE:
                    snprintf(country_attribute, AMBER_MAX_ATTRIBUTE_STRING, ",%s down cache", options->country);
                    break;
                case AMBER_ACTION_NONE:
                    break;
            }
        }
        if (strlen(country_attribute)) {
            strncat((char *) out, country_attribute, AMBER_MAX_ATTRIBUTE_STRING - strlen((char *)out));
        }
    }

    return 0;
}



