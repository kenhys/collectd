/**
 * collectd - src/utils/value_list/value_list.c
 * Copyright (C) 2005-2023  Florian octo Forster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 *   Sebastian Harl <sh at tokkee.org>
 *   Manoj Srivastava <srivasta at google.com>
 **/

#include "collectd.h"
#include "daemon/plugin.h"
#include "utils/common/common.h"
#include "utils/value_list/value_list.h"

#ifdef WIN32
#define EXPORT __declspec(dllexport)
#include <sys/stat.h>
#include <unistd.h>
#else
#define EXPORT
#endif

EXPORT int plugin_dispatch_values(value_list_t const *vl) {
  data_set_t const *ds = plugin_get_ds(vl->type);
  if (ds == NULL) {
    return EINVAL;
  }

  for (size_t i = 0; i < vl->values_len; i++) {
    metric_family_t *fam = plugin_value_list_to_metric_family(vl, ds, i);
    if (fam == NULL) {
      int status = errno;
      ERROR("plugin_dispatch_values: plugin_value_list_to_metric_family "
            "failed: %s",
            STRERROR(status));
      return status;
    }

    int status = plugin_dispatch_metric_family(fam);
    metric_family_free(fam);
    if (status != 0) {
      return status;
    }
  }

  return 0;
}

static void plugin_value_list_free(value_list_t *vl) /* {{{ */
{
  if (vl == NULL)
    return;

  meta_data_destroy(vl->meta);
  sfree(vl->values);
  sfree(vl);
} /* }}} void plugin_value_list_free */

static value_list_t *
plugin_value_list_clone(value_list_t const *vl_orig) /* {{{ */
{
  value_list_t *vl;

  if (vl_orig == NULL)
    return NULL;

  vl = malloc(sizeof(*vl));
  if (vl == NULL)
    return NULL;
  memcpy(vl, vl_orig, sizeof(*vl));

  if (vl->host[0] == 0)
    sstrncpy(vl->host, hostname_g, sizeof(vl->host));

  vl->values = calloc(vl_orig->values_len, sizeof(*vl->values));
  if (vl->values == NULL) {
    plugin_value_list_free(vl);
    return NULL;
  }
  memcpy(vl->values, vl_orig->values,
         vl_orig->values_len * sizeof(*vl->values));

  vl->meta = meta_data_clone(vl->meta);
  if ((vl_orig->meta != NULL) && (vl->meta == NULL)) {
    plugin_value_list_free(vl);
    return NULL;
  }

  if (vl->time == 0)
    vl->time = cdtime();

  /* Fill in the interval from the thread context, if it is zero. */
  if (vl->interval == 0)
    vl->interval = plugin_get_interval();

  return vl;
} /* }}} value_list_t *plugin_value_list_clone */

__attribute__((sentinel)) int
plugin_dispatch_multivalue(value_list_t const *template, /* {{{ */
                           bool store_percentage, int store_type, ...) {
  value_list_t *vl;
  int failed = 0;
  gauge_t sum = 0.0;
  va_list ap;

  assert(template->values_len == 1);

  /* Calculate sum for Gauge to calculate percent if needed */
  if (DS_TYPE_GAUGE == store_type) {
    va_start(ap, store_type);
    while (42) {
      char const *name;
      gauge_t value;

      name = va_arg(ap, char const *);
      if (name == NULL)
        break;

      value = va_arg(ap, gauge_t);
      if (!isnan(value))
        sum += value;
    }
    va_end(ap);
  }

  vl = plugin_value_list_clone(template);
  /* plugin_value_list_clone makes sure vl->time is set to non-zero. */
  if (store_percentage)
    sstrncpy(vl->type, "percent", sizeof(vl->type));

  va_start(ap, store_type);
  while (42) {
    char const *name;
    int status;

    /* Set the type instance. */
    name = va_arg(ap, char const *);
    if (name == NULL)
      break;
    sstrncpy(vl->type_instance, name, sizeof(vl->type_instance));

    /* Set the value. */
    switch (store_type) {
    case DS_TYPE_GAUGE:
      vl->values[0].gauge = va_arg(ap, gauge_t);
      if (store_percentage)
        vl->values[0].gauge *= sum ? (100.0 / sum) : NAN;
      break;
    case DS_TYPE_COUNTER:
      vl->values[0].counter = va_arg(ap, counter_t);
      break;
    case DS_TYPE_DERIVE:
      vl->values[0].derive = va_arg(ap, derive_t);
      break;
    default:
      ERROR("plugin_dispatch_multivalue: given store_type is incorrect.");
      failed++;
    }

    status = plugin_dispatch_values(vl);
    if (status != 0)
      failed++;
  }
  va_end(ap);

  plugin_value_list_free(vl);
  return failed;
} /* }}} int plugin_dispatch_multivalue */

static int metric_family_name(strbuf_t *buf, value_list_t const *vl,
                              data_source_t const *dsrc) {
  int status = strbuf_print(buf, "collectd");

  if (strcmp(vl->plugin, vl->type) != 0) {
    status = status || strbuf_print(buf, "_");
    status = status || strbuf_print(buf, vl->plugin);
  }

  status = status || strbuf_print(buf, "_");
  status = status || strbuf_print(buf, vl->type);

  if (strcmp("value", dsrc->name) != 0) {
    status = status || strbuf_print(buf, "_");
    status = status || strbuf_print(buf, dsrc->name);
  }

  if ((dsrc->type == DS_TYPE_COUNTER) || (dsrc->type == DS_TYPE_DERIVE)) {
    status = status || strbuf_print(buf, "_total");
  }

  return status;
}

int parse_identifier(char *str, char **ret_host, char **ret_plugin,
                     char **ret_type, char **ret_data_source,
                     char *default_host) {
  char *fields[5];
  size_t fields_num = 0;

  do {
    fields[fields_num] = str;
    fields_num++;

    char *ptr = strchr(str, '/');
    if (ptr == NULL) {
      break;
    }

    *ptr = 0;
    str = ptr + 1;
  } while (fields_num < STATIC_ARRAY_SIZE(fields));

  switch (fields_num) {
  case 4:
    *ret_data_source = fields[3];
    /* fall-through */
  case 3:
    *ret_type = fields[2];
    *ret_plugin = fields[1];
    *ret_host = fields[0];
    break;
  case 2:
    if ((default_host == NULL) || (strlen(default_host) == 0)) {
      return EINVAL;
    }
    *ret_type = fields[1];
    *ret_plugin = fields[0];
    *ret_host = default_host;
    break;
  default:
    return EINVAL;
  }

  return 0;
} /* int parse_identifier */

int parse_identifier_vl(const char *str, value_list_t *vl,
                        char **ret_data_source) {
  if ((str == NULL) || (vl == NULL))
    return EINVAL;

  char str_copy[6 * DATA_MAX_NAME_LEN];
  sstrncpy(str_copy, str, sizeof(str_copy));

  char *default_host = NULL;
  if (strlen(vl->host) != 0) {
    default_host = vl->host;
  }

  char *host = NULL;
  char *plugin = NULL;
  char *type = NULL;
  char *data_source = NULL;
  int status = parse_identifier(str_copy, &host, &plugin, &type, &data_source,
                                default_host);
  if (status != 0) {
    return status;
  }

  if (data_source != NULL) {
    if (ret_data_source == NULL) {
      return EINVAL;
    }
    *ret_data_source = strdup(data_source);
  }

  char *plugin_instance = strchr(plugin, '-');
  if (plugin_instance != NULL) {
    *plugin_instance = 0;
    plugin_instance++;
  }
  char *type_instance = strchr(type, '-');
  if (type_instance != NULL) {
    *type_instance = 0;
    type_instance++;
  }

  if (host != vl->host) {
    sstrncpy(vl->host, host, sizeof(vl->host));
  }
  sstrncpy(vl->plugin, plugin, sizeof(vl->plugin));
  if (plugin_instance != NULL) {
    sstrncpy(vl->plugin_instance, plugin_instance, sizeof(vl->plugin_instance));
  }
  sstrncpy(vl->type, type, sizeof(vl->type));
  if (type_instance != NULL) {
    sstrncpy(vl->type_instance, type_instance, sizeof(vl->type_instance));
  }

  return 0;
} /* }}} int parse_identifier_vl */

metric_t *parse_legacy_identifier(char const *s) {
  value_list_t vl = VALUE_LIST_INIT;

  char *data_source = NULL;
  int status = parse_identifier_vl(s, &vl, &data_source);
  if (status != 0) {
    errno = status;
    return NULL;
  }

  data_set_t const *ds = plugin_get_ds(vl.type);
  if (ds == NULL) {
    errno = ENOENT;
    return NULL;
  }

  if ((ds->ds_num != 1) && (data_source == NULL)) {
    DEBUG("parse_legacy_identifier: data set \"%s\" has multiple data sources, "
          "but \"%s\" does not specify a data source",
          ds->type, s);
    errno = EINVAL;
    return NULL;
  }

  value_t values[ds->ds_num];
  memset(values, 0, sizeof(values));
  vl.values = values;
  vl.values_len = ds->ds_num;

  size_t ds_index = 0;
  if (data_source != NULL) {
    bool found = 0;
    for (size_t i = 0; i < ds->ds_num; i++) {
      if (strcasecmp(data_source, ds->ds[i].name) == 0) {
        ds_index = i;
        found = true;
      }
    }

    if (!found) {
      DEBUG("parse_legacy_identifier: data set \"%s\" does not have a \"%s\" "
            "data source",
            ds->type, data_source);
      free(data_source);
      errno = EINVAL;
      return NULL;
    }
  }
  free(data_source);
  data_source = NULL;

  metric_family_t *fam = plugin_value_list_to_metric_family(&vl, ds, ds_index);
  if (fam == NULL) {
    return NULL;
  }

  return fam->metric.ptr;
}

EXPORT metric_family_t *
plugin_value_list_to_metric_family(value_list_t const *vl, data_set_t const *ds,
                                   size_t index) {
  if ((vl == NULL) || (ds == NULL)) {
    errno = EINVAL;
    return NULL;
  }

  metric_family_t *fam = calloc(1, sizeof(*fam));
  if (fam == NULL) {
    return NULL;
  }

  data_source_t const *dsrc = ds->ds + index;
  strbuf_t name = STRBUF_CREATE;
  int status = metric_family_name(&name, vl, dsrc);
  if (status != 0) {
    STRBUF_DESTROY(name);
    metric_family_free(fam);
    errno = status;
    return NULL;
  }
  fam->name = name.ptr;
  name = (strbuf_t){0};

  fam->type =
      (dsrc->type == DS_TYPE_GAUGE) ? METRIC_TYPE_GAUGE : METRIC_TYPE_COUNTER;

  metric_t m = {
      .family = fam,
      .value = vl->values[index],
      .time = vl->time,
      .interval = vl->interval,
  };

  status = metric_label_set(&m, "instance",
                            (strlen(vl->host) != 0) ? vl->host : hostname_g);
  if (strlen(vl->plugin_instance) != 0) {
    status = status || metric_label_set(&m, vl->plugin, vl->plugin_instance);
  }
  if (strlen(vl->type_instance) != 0) {
    char const *name = "type";
    if (strlen(vl->plugin_instance) == 0) {
      name = vl->plugin;
    }
    status = status || metric_label_set(&m, name, vl->type_instance);
  }

  status = status || metric_family_metric_append(fam, m);
  if (status != 0) {
    metric_reset(&m);
    metric_family_free(fam);
    errno = status;
    return NULL;
  }

  metric_reset(&m);
  return fam;
}
