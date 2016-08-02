/*
 * Copyright (C) 2014 Kobi Meirson
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "cJSON/cJSON.h"

typedef struct {
	ngx_regex_compile_t *rgc;
	ngx_str_t			*replacement;
  ngx_str_t     *ver_replacement;
} ngx_http_ua_parse_elem_t;

typedef struct {
    ngx_array_t *devices;
    ngx_array_t *browsers;
    ngx_array_t *os;
    ngx_array_t *brands;
    ngx_array_t *models;
    // Kind regexes
    ngx_regex_t tabletKindRegex;
    ngx_regex_t mobileKindRegex;
} ngx_http_ua_parse_conf_t;

static ngx_str_t *ngx_http_ua_copy_json(cJSON *jsonSrc, ngx_conf_t *cf);

static ngx_int_t ngx_http_ua_parse_add_variables(ngx_conf_t *cf);

static char *ngx_http_ua_parse_list(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_int_t ngx_http_ua_parse_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

static ngx_int_t ngx_http_ua_parse_kind_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

static void * ngx_http_ua_parse_create_conf(ngx_conf_t *cf);
static char * ngx_http_ua_parse_init_conf(ngx_conf_t *cf, void *conf);
static void ngx_http_ua_parse_cleanup(void *data);

static ngx_array_t * ngx_http_ua_parse_load_from_json(ngx_conf_t *cf, cJSON *current);

static ngx_command_t ngx_http_ua_parse_commands[] = {
    { ngx_string("uaparse_list"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_ua_parse_list,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    ngx_null_command
};

static ngx_http_module_t ngx_http_ua_parse_module_ctx = {
    ngx_http_ua_parse_add_variables,    /* preconfiguration */
    NULL,                               /* postconfiguration */

    ngx_http_ua_parse_create_conf,      /* create main configuration */
    ngx_http_ua_parse_init_conf,        /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    NULL,                               /* create location configuration */
    NULL                                /* merge location configuration */
};


ngx_module_t ngx_http_ua_parse_module = {
    NGX_MODULE_V1,
    &ngx_http_ua_parse_module_ctx, /* module context */
    ngx_http_ua_parse_commands,    /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

#define NGX_UA_PARSE_DEVICE_FAMILY 0
#define NGX_UA_PARSE_OS_FAMILY 1
#define NGX_UA_PARSE_BROWSER_FAMILY 2
#define NGX_UA_PARSE_BROWSER_VERSION 3
#define NGX_UA_PARSE_DEVICE_BRAND 4
#define NGX_UA_PARSE_DEVICE_MODEL 5
#define NGX_UA_PARSE_OS_VERSION 6

#define NGX_UA_PARSE_SIZE_THRESHOLD 200

static ngx_http_variable_t ngx_http_ua_parse_vars[] = {
    { ngx_string("ua_parse_device"), NULL,
      ngx_http_ua_parse_variable,
      NGX_UA_PARSE_DEVICE_FAMILY, 0, 0 },

    { ngx_string("ua_parse_os"), NULL,
      ngx_http_ua_parse_variable,
      NGX_UA_PARSE_OS_FAMILY, 0, 0 },

    { ngx_string("ua_parse_os_ver"), NULL,
      ngx_http_ua_parse_variable,
      NGX_UA_PARSE_OS_VERSION, 0, 0 },

    { ngx_string("ua_parse_browser"), NULL,
      ngx_http_ua_parse_variable,
      NGX_UA_PARSE_BROWSER_FAMILY, 0, 0 },

    { ngx_string("ua_parse_browser_ver"), NULL,
      ngx_http_ua_parse_variable,
      NGX_UA_PARSE_BROWSER_VERSION, 0, 0 },

    { ngx_string("ua_parse_device_kind"), NULL,
      ngx_http_ua_parse_kind_variable,
      0, 0, 0 },

    { ngx_string("ua_parse_device_brand"), NULL,
      ngx_http_ua_parse_variable,
      NGX_UA_PARSE_DEVICE_BRAND, 0, 0 },

    { ngx_string("ua_parse_device_model"), NULL,
      ngx_http_ua_parse_variable,
      NGX_UA_PARSE_DEVICE_MODEL, 0, 0 },

    { ngx_null_string, NULL, NULL, 0, 0, 0 }
};

// Copy json attributes (just to be able to destroy the cJSON obj afterwards)
static ngx_str_t *
ngx_http_ua_copy_json(cJSON *jsonSrc, ngx_conf_t *cf) {
    ngx_str_t     *str;
    u_char        *src = (u_char*)jsonSrc->valuestring;

    str = ngx_pcalloc(cf->pool, sizeof(ngx_str_t));
    str->len = ngx_strlen(src) + 1;
    str->data = ngx_pcalloc(cf->pool, str->len);
    (void)ngx_copy(str->data, src, str->len);

    return str;
}

// Create configuration
static void *
ngx_http_ua_parse_create_conf(ngx_conf_t *cf)
{
    ngx_pool_cleanup_t     *cln;
    ngx_http_ua_parse_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_ua_parse_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    cln = ngx_pool_cleanup_add(cf->pool, 0);
    if (cln == NULL) {
        return NULL;
    }

    cln->handler = ngx_http_ua_parse_cleanup;
    cln->data = conf;

    return conf;
}

// Init configuration
static char *
ngx_http_ua_parse_init_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_ua_parse_conf_t    *upcf = conf;
    ngx_regex_compile_t			rgc;
    char						*rc;
    u_char						errstr[NGX_MAX_CONF_ERRSTR];

    rc = NGX_CONF_ERROR;

    // Mobile
	rgc.pattern = (ngx_str_t)ngx_string("iphone|ipod|mobile");
	rgc.options = NGX_REGEX_CASELESS;
	rgc.pool = cf->pool;
	rgc.err.len = NGX_MAX_CONF_ERRSTR;
	rgc.err.data = errstr;
	if (ngx_regex_compile(&rgc) != NGX_OK) {
		ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
								  "ngx_regex_compile() \"%s\" failed", rgc.pattern.data);
		goto failed;
	}
	ngx_memcpy(&upcf->mobileKindRegex, rgc.regex, sizeof(ngx_regex_t));

	// Tablet
	rgc.pattern = (ngx_str_t)ngx_string("ipad");
	rgc.options = NGX_REGEX_CASELESS;
	rgc.pool = cf->pool;
	rgc.err.len = NGX_MAX_CONF_ERRSTR;
	rgc.err.data = errstr;
	if (ngx_regex_compile(&rgc) != NGX_OK) {
		ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
		                          "ngx_regex_compile() \"%s\" failed", rgc.pattern.data);
		goto failed;
	}
	ngx_memcpy(&upcf->tabletKindRegex, rgc.regex, sizeof(ngx_regex_t));

	rc = NGX_CONF_OK;

failed:
	return rc;
}

// Kind (other/mobile/tablet)
static ngx_int_t ngx_http_ua_parse_kind_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_ua_parse_conf_t    *upcf;
	u_char						*str;
	ngx_http_variable_value_t	device, os;
	ngx_regex_t					*mobileKind, *tabletKind;

	upcf = ngx_http_get_module_main_conf(r, ngx_http_ua_parse_module);
	mobileKind = &upcf->mobileKindRegex;
	tabletKind = &upcf->tabletKindRegex;

	str = (u_char*) "other";

	ngx_http_ua_parse_variable(r, &os, NGX_UA_PARSE_OS_FAMILY);
	if (os.valid == 1) {
		if (ngx_strstr(os.data, "iOS") != NULL) {
			ngx_http_ua_parse_variable(r, &device, NGX_UA_PARSE_DEVICE_FAMILY);
			if (device.valid == 1) {
				// In ios, we check the device
				if (ngx_regex_exec(mobileKind, &device, NULL, 0) >= 0) { // We can use 'device' as ngx_string_t, as ngx_http_variable_value_t shares ->len and ->data
					str = (u_char*)"mobile";
				} else if (ngx_regex_exec(tabletKind, &device, NULL, 0) >= 0) {
					str = (u_char*)"tablet";
				}
			}
		} else if (ngx_strstr(os.data, "Android") != NULL) {
			// In android - we check the UA for "mobile"
			if (ngx_regex_exec(mobileKind, &(r->headers_in.user_agent->value), NULL, 0) >= 0) {
				str = (u_char*)"mobile";
			} else {
				str = (u_char*)"tablet";
			}
		}
	}

	v->data = str;
	v->len = ngx_strlen(v->data);
	v->valid = 1;
	v->no_cacheable = 0;
	v->not_found = 0;

	return NGX_OK;
}

// Get family
static ngx_int_t ngx_http_ua_parse_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_ua_parse_conf_t    *upcf;
    ngx_uint_t                  n, i, captures_amount, replacement_len = 0;
    ngx_http_ua_parse_elem_t   	*ptr, *cur;
    int                         rc, *captures = NULL;
    ngx_array_t					*lst;
    ngx_str_t					str;
    u_char						*p, *foundStr;

    upcf = ngx_http_get_module_main_conf(r, ngx_http_ua_parse_module);
    v->valid = 0;

    switch (data) {
    case NGX_UA_PARSE_DEVICE_FAMILY:
    	lst = upcf->devices;
    	break;
    case NGX_UA_PARSE_OS_FAMILY:
    	lst = upcf->os;
    	break;
    case NGX_UA_PARSE_OS_VERSION:
      lst = upcf->os;
      break;
    case NGX_UA_PARSE_BROWSER_FAMILY:
    	lst = upcf->browsers;
    	break;
    case NGX_UA_PARSE_BROWSER_VERSION:
      lst = upcf->browsers;
      break;
    case NGX_UA_PARSE_DEVICE_BRAND:
      lst = upcf->brands;
      break;
    case NGX_UA_PARSE_DEVICE_MODEL:
      lst = upcf->models;
      break;
    default:
    	goto not_found;
    }

    if (lst == NULL) {
        goto not_found;
    }

    if (!r->headers_in.user_agent) {
      goto not_found;
    }

    ptr = lst->elts;
    for (i = 0; i < lst->nelts; i++) {
        cur = &ptr[i];
        n = (cur->rgc->captures + 1) * 3;
        if (n > 20) {
        	n = 15; // 4+1 * 3
          captures_amount = 4;
        } else {
          captures_amount = cur->rgc->captures;
        }
        captures = ngx_palloc(r->pool, n * sizeof(int));
        rc = ngx_regex_exec(cur->rgc->regex, &r->headers_in.user_agent->value, captures, n);
        if (rc < 0) {
        	continue;
        }

        // if we have 0 captures, we have no right to access captures[3], and captures[2] contain no meaningful info
        // so we just take the whole match (captures[1] - captures[0])
        str.data = (u_char *) (r->headers_in.user_agent->value.data + captures[0]);
        str.len = captures[1] - captures[0];

        // Match the first one (captures[2] is the start, captures[3] is the end) in most conditions
        if (captures_amount > 0 && (captures[3] - captures[2] < 0 && str.len > NGX_UA_PARSE_SIZE_THRESHOLD)) {
          str.data = (u_char *) (r->headers_in.user_agent->value.data + captures[2]);
          str.len = captures[3] - captures[2];
        }

        if ((data == NGX_UA_PARSE_BROWSER_VERSION || data == NGX_UA_PARSE_OS_VERSION) && cur->rgc->captures > 1) {
          while (captures_amount > 1) {
            if (captures[captures_amount * 2 + 1] != -1) {
              str.data = (u_char *) (r->headers_in.user_agent->value.data + captures[4]);
              str.len = captures[captures_amount * 2 + 1] - captures[4];
              break;
            } else {
              captures_amount = captures_amount - 1;
            }
          }
        }

        // we use all matches since we'll most likely replace them with something later on
        if (data == NGX_UA_PARSE_DEVICE_MODEL || data == NGX_UA_PARSE_DEVICE_BRAND) {
          while (captures_amount > 1) {
            if (captures[captures_amount * 2 + 1] != - 1) {
              str.data = (u_char *) (r->headers_in.user_agent->value.data + captures[2]);
              str.len = captures[captures_amount * 2 + 1] - captures[2];
              break;
            } else {
              captures_amount = captures_amount - 1;
            }
          }
        }

        if ((cur->replacement && data != NGX_UA_PARSE_BROWSER_VERSION && data != NGX_UA_PARSE_OS_VERSION) || (cur->ver_replacement && (data == NGX_UA_PARSE_OS_VERSION))) {
        	// Copy the string to the foundStr place...
        	foundStr = ngx_palloc(r->pool, (str.len + 1) * sizeof(u_char));
        	ngx_memzero(foundStr, (str.len + 1) * sizeof(u_char)); // Make sure there will be '\0' in the end
        	ngx_memcpy(foundStr, str.data, str.len * sizeof(u_char));

        	// Now we can use sprintf() safely (else it would copy the whole user agent string..)
          if (data == NGX_UA_PARSE_OS_VERSION) {
            replacement_len = cur->ver_replacement->len;
          } else {
            replacement_len = cur->replacement->len;
          }
          str.data = p = ngx_palloc(r->pool, (replacement_len + str.len + 1) * sizeof(u_char));
          ngx_memzero(p, (replacement_len + str.len + 1) * sizeof(u_char));

          if (data == NGX_UA_PARSE_OS_VERSION) {
            p = ngx_snprintf(p, (size_t) (replacement_len + str.len + 1) * sizeof(u_char), (const char *)cur->ver_replacement->data, foundStr);
          } else {
            p = ngx_snprintf(p, (size_t) (replacement_len + str.len + 1) * sizeof(u_char), (const char *)cur->replacement->data, foundStr);
          }
        	*p = '\0';
        	str.len = p - str.data;

          // Finally free foundStr after we've used it
          ngx_pfree(r->pool, foundStr);
        }

        v->data = str.data;
		v->len = str.len;
		v->valid = 1;
		v->no_cacheable = 0;
		v->not_found = 0;
		break;
    }

not_found:
	if (v->valid != 1) {
		v->not_found = 1;
	}

  if (captures) {
    ngx_pfree(r->pool, captures);
  }
    return NGX_OK;
}

static ngx_array_t * ngx_http_ua_parse_load_from_json(ngx_conf_t *cf, cJSON *current)
{
	int            				i, arraySize;
	ngx_array_t					*lst;
	cJSON						*arr;
	ngx_http_ua_parse_elem_t	*elem;
	ngx_str_t					*str;
	u_char						errstr[NGX_MAX_CONF_ERRSTR];

	arraySize = cJSON_GetArraySize(current);
	lst = ngx_array_create(cf->pool, arraySize, sizeof(ngx_http_ua_parse_elem_t));
	if (lst == NULL) {
		goto failed;
	}
	for (i = 0; i < arraySize; i++) {
		arr = cJSON_GetArrayItem(current, i);
		elem = ngx_array_push(lst);
		if (elem == NULL) {
			goto failed;
		}
        str = ngx_http_ua_copy_json(cJSON_GetObjectItem(arr, "regex"), cf);

		elem->rgc = ngx_calloc(sizeof(ngx_regex_compile_t), cf->log);
		ngx_memzero(elem->rgc, sizeof(ngx_regex_compile_t));

		elem->rgc->pattern = *str;
		elem->rgc->pool = cf->pool;
		elem->rgc->err.len = NGX_MAX_CONF_ERRSTR;
		elem->rgc->err.data = errstr;
		if (ngx_regex_compile(elem->rgc) != NGX_OK) {
			goto failed;
		}
		if (cJSON_GetObjectItem(arr, "replacement") != NULL) {
            elem->replacement = ngx_http_ua_copy_json(cJSON_GetObjectItem(arr, "replacement"), cf);
		}
    if (cJSON_GetObjectItem(arr, "version_replacement") != NULL) {
      elem->ver_replacement = ngx_http_ua_copy_json(cJSON_GetObjectItem(arr, "version_replacement"), cf);
    }
	}
failed:
	return lst;
}

static char *
ngx_http_ua_parse_list(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_fd_t        fd = NGX_INVALID_FILE;
    ngx_str_t       *value;
    u_char          *regexFile;
    char            *buf = NULL;
    size_t          len;
    ngx_file_info_t fi;
    off_t           size;
    char*           rc;
    cJSON           *root = NULL;
    ngx_http_ua_parse_conf_t    *upcf = conf;

    rc = NGX_CONF_ERROR;

    if (upcf->devices) {
        return "duplicate!";
    }

    value = cf->args->elts;
    regexFile = (u_char*)value[1].data;

    fd = ngx_open_file(regexFile, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_CRIT, cf->log, ngx_errno,
                      ngx_open_file_n " \"%s\" failed", regexFile);
    }

    if (ngx_fd_info(fd, &fi) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
                      ngx_fd_info_n " \"%s\" failed", regexFile);
        goto failed;
    }
    size = ngx_file_size(&fi);

    // TODO: this value is hardcoded. Is this really the only way to go? Slight changes to json db will definitely disturb the process
    len = (off_t) 174781 > size ? (size_t) size : 174781;

    buf = ngx_alloc(len, cf->log);
    if (buf == NULL) {
        goto failed;
    }

    if (ngx_read_fd(fd, buf, len) == -1) {
    	goto failed;
    }


    root = cJSON_Parse(buf);

    // OS
    // ngx_http_ua_parse_elem_t
    upcf->os = ngx_http_ua_parse_load_from_json(cf, cJSON_GetObjectItem(root, "os"));
    upcf->devices = ngx_http_ua_parse_load_from_json(cf, cJSON_GetObjectItem(root, "devices"));
    upcf->browsers = ngx_http_ua_parse_load_from_json(cf, cJSON_GetObjectItem(root, "browsers"));
    upcf->brands = ngx_http_ua_parse_load_from_json(cf, cJSON_GetObjectItem(root, "brands"));
    upcf->models= ngx_http_ua_parse_load_from_json(cf, cJSON_GetObjectItem(root, "models"));

    rc = NGX_CONF_OK;

failed:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (fd != NGX_INVALID_FILE) {
        if (ngx_close_file(fd) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
                          ngx_close_file_n " \"%s\" failed", regexFile);
        }
    }
    if (buf != NULL) {
        ngx_free(buf);
    }
    return rc;
}

static ngx_int_t
ngx_http_ua_parse_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t *var, *v;

    for (v = ngx_http_ua_parse_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }
        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}

static void
ngx_http_ua_parse_cleanup(void *data)
{
    ngx_http_ua_parse_conf_t  *upcf = data;

    if (upcf->devices) {
        ngx_array_destroy(upcf->devices);
    }

    if (upcf->browsers) {
        ngx_array_destroy(upcf->browsers);
    }

    if (upcf->os) {
        ngx_array_destroy(upcf->os);
    }

    if (upcf->brands) {
      ngx_array_destroy(upcf->brands);
    }

    if (upcf->models) {
      ngx_array_destroy(upcf->models);
    }

}
