/**
 * collectd - src/utils_lua.c
 * Copyright (C) 2010       Florian Forster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Florian Forster <octo at collectd.org>
 **/

#include "utils/common/common.h"
#include "utils_lua.h"

static int ltoc_values(lua_State *L, /* {{{ */
                       const data_set_t *ds, value_t *ret_values) {
  if (!lua_istable(L, -1)) {
    WARNING("ltoc_values: not a table");
    return -1;
  }

  /* Push initial key */
  lua_pushnil(L); /* +1 = 1 */
  size_t i = 0;
  while (lua_next(L, -2) != 0) /* -1+2 = 2 || -1 = 0 */
  {
    if (i >= ds->ds_num) {
      lua_pop(L, 2); /* -2 = 0 */
      i++;
      break;
    }

    ret_values[i] = luaC_tovalue(L, -1, ds->ds[i].type);

    /* Pop the value */
    lua_pop(L, 1); /* -1 = 1 */
    i++;
  } /* while (lua_next) */

  if (i != ds->ds_num) {
    WARNING("ltoc_values: invalid size for datasource \"%s\": expected %" PRIsz
            ", got %" PRIsz,
            ds->type, ds->ds_num, i);
    return -1;
  }

  return 0;
} /* }}} int ltoc_values */

static int ltoc_table_values(lua_State *L, int idx, /* {{{ */
                             const data_set_t *ds, value_list_t *vl) {
  /* We're only called from "luaC_tovaluelist", which ensures that "idx" is an
   * absolute index (i.e. a positive number) */
  assert(idx > 0);

  lua_getfield(L, idx, "values");
  if (!lua_istable(L, -1)) {
    WARNING("utils_lua: ltoc_table_values: The \"values\" member is a %s "
            "value, not a table.",
            lua_typename(L, lua_type(L, -1)));
    lua_pop(L, 1);
    return -1;
  }

  vl->values_len = ds->ds_num;
  vl->values = calloc(vl->values_len, sizeof(*vl->values));
  if (vl->values == NULL) {
    ERROR("utils_lua: calloc failed.");
    vl->values_len = 0;
    lua_pop(L, 1);
    return -1;
  }

  int status = ltoc_values(L, ds, vl->values);

  lua_pop(L, 1);

  if (status != 0) {
    vl->values_len = 0;
    sfree(vl->values);
  }

  return status;
} /* }}} int ltoc_table_values */

/*
 * Public functions
 */
cdtime_t luaC_tocdtime(lua_State *L, int idx) /* {{{ */
{
  if (!lua_isnumber(L, /* stack pos = */ idx))
    return 0;

  double d = lua_tonumber(L, idx);

  return DOUBLE_TO_CDTIME_T(d);
} /* }}} int ltoc_table_cdtime */

int luaC_tostringbuffer(lua_State *L, int idx, /* {{{ */
                        char *buffer, size_t buffer_size) {
  const char *str = lua_tostring(L, idx);
  if (str == NULL)
    return -1;

  sstrncpy(buffer, str, buffer_size);
  return 0;
} /* }}} int luaC_tostringbuffer */

value_t luaC_tovalue(lua_State *L, int idx, int ds_type) /* {{{ */
{
  value_t v = {0};

  if (!lua_isnumber(L, idx))
    return v;

  if (ds_type == DS_TYPE_GAUGE)
    v.gauge = (gauge_t)lua_tonumber(L, /* stack pos = */ -1);
  else if (ds_type == DS_TYPE_DERIVE)
    v.derive = (derive_t)lua_tointeger(L, /* stack pos = */ -1);
  else if (ds_type == DS_TYPE_COUNTER)
    v.counter = (counter_t)lua_tointeger(L, /* stack pos = */ -1);

  return v;
} /* }}} value_t luaC_tovalue */

value_list_t *luaC_tovaluelist(lua_State *L, int idx) /* {{{ */
{
#if COLLECT_DEBUG
  int stack_top_before = lua_gettop(L);
#endif

  /* Convert relative indexes to absolute indexes, so it doesn't change when we
   * push / pop stuff. */
  if (idx < 1)
    idx += lua_gettop(L) + 1;

  /* Check that idx is in the valid range */
  if ((idx < 1) || (idx > lua_gettop(L))) {
    DEBUG("luaC_tovaluelist: idx(%d), top(%d)", idx, stack_top_before);
    return NULL;
  }

  value_list_t *vl = calloc(1, sizeof(*vl));
  if (vl == NULL) {
    DEBUG("luaC_tovaluelist: calloc failed");
    return NULL;
  }

  /* Push initial key */
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    const char *key = lua_tostring(L, -2);

    if (key == NULL) {
      DEBUG("luaC_tovaluelist: Ignoring non-string key.");
    } else if (strcasecmp("host", key) == 0)
      luaC_tostringbuffer(L, -1, vl->host, sizeof(vl->host));
    else if (strcasecmp("plugin", key) == 0)
      luaC_tostringbuffer(L, -1, vl->plugin, sizeof(vl->plugin));
    else if (strcasecmp("plugin_instance", key) == 0)
      luaC_tostringbuffer(L, -1, vl->plugin_instance,
                          sizeof(vl->plugin_instance));
    else if (strcasecmp("type", key) == 0)
      luaC_tostringbuffer(L, -1, vl->type, sizeof(vl->type));
    else if (strcasecmp("type_instance", key) == 0)
      luaC_tostringbuffer(L, -1, vl->type_instance, sizeof(vl->type_instance));
    else if (strcasecmp("time", key) == 0)
      vl->time = luaC_tocdtime(L, -1);
    else if (strcasecmp("interval", key) == 0)
      vl->interval = luaC_tocdtime(L, -1);
    else if (strcasecmp("values", key) == 0) {
      /* This key is not handled here, because we have to assure "type" is read
       * first. */
    } else {
      DEBUG("luaC_tovaluelist: Ignoring unknown key \"%s\".", key);
    }

    /* Pop the value */
    lua_pop(L, 1);
  }

  const data_set_t *ds = plugin_get_ds(vl->type);
  if (ds == NULL) {
    INFO("utils_lua: Unable to lookup type \"%s\".", vl->type);
    sfree(vl);
    return NULL;
  }

  int status = ltoc_table_values(L, idx, ds, vl);
  if (status != 0) {
    WARNING("utils_lua: ltoc_table_values failed.");
    sfree(vl);
    return NULL;
  }

#if COLLECT_DEBUG
  assert(stack_top_before == lua_gettop(L));
#endif
  return vl;
} /* }}} value_list_t *luaC_tovaluelist */

int luaC_pushcdtime(lua_State *L, cdtime_t t) /* {{{ */
{
  double d = CDTIME_T_TO_DOUBLE(t);

  lua_pushnumber(L, (lua_Number)d);
  return 0;
} /* }}} int luaC_pushcdtime */

int luaC_pushvalue(lua_State *L, value_t v, int ds_type) /* {{{ */
{
  if (ds_type == DS_TYPE_GAUGE)
    lua_pushnumber(L, (lua_Number)v.gauge);
  else if (ds_type == DS_TYPE_DERIVE)
    lua_pushinteger(L, (lua_Integer)v.derive);
  else if (ds_type == DS_TYPE_COUNTER)
    lua_pushinteger(L, (lua_Integer)v.counter);
  else
    return -1;
  return 0;
} /* }}} int luaC_pushvalue */

int luaC_pushmetricfamily(lua_State *L, metric_family_t const *mf) /* {{{ */
{
  DEBUG("lua: luaC_pushmetricfamily called");

  /*
    metric_family_t is mapped to the following Lua table:

    {
      name => ...,
      help => ...,
      type => ...,
      metric => {
       [0] => {
         label => {
           [0] => {
           },
           ...
           [N] => {
           }
         },
         value => ,
         time => ,
         interval => ,
         meta =>
       },
       ...
       [N] => {
         ...
       }
    }
  */

  lua_newtable(L);

  lua_pushstring(L, mf->name);
  lua_setfield(L, -2, "name");

  lua_pushstring(L, mf->help);
  lua_setfield(L, -2, "help");

  lua_pushinteger(L, (lua_Integer)mf->type);
  lua_setfield(L, -2, "type");

  if (mf->metric.num > 0) {
    /* metric = {} */
    lua_newtable(L);
    DEBUG("lua: top: %d", lua_gettop(L));
    DEBUG("lua: metric.num: %zd", mf->metric.num);
    metric_t *ptr = mf->metric.ptr;
    for (int i = 0; i < mf->metric.num; i++) {
      DEBUG("lua: metric[%d]", i);
      /* metric[N] = {} */
      lua_newtable(L);
      if (ptr->label.num > 0) {
        label_pair_t *label_ptr = ptr->label.ptr;
        /* label = {} */
        lua_newtable(L);
        for (int j = 0; j < ptr->label.num; j++) {
          DEBUG("lua: metric[%d].label[%d]", i, j);
          /* { name = value } */
          lua_newtable(L);
          lua_pushstring(L, label_ptr->value);
          lua_setfield(L, -2, label_ptr->name);
          lua_rawseti(L, -2, j + 1);
          label_ptr++;
          DEBUG("lua: top: %d", lua_gettop(L));
        }
        lua_setfield(L, -2, "label");
      }
      switch (mf->type) {
      case METRIC_TYPE_COUNTER:
        DEBUG("lua: metric[%d]", i);
        lua_pushinteger(L, (lua_Integer)ptr->value.counter);
        lua_setfield(L, -2, "counter");
        break;
      case METRIC_TYPE_GAUGE:
        lua_pushnumber(L, ptr->value.gauge);
        lua_setfield(L, -2, "gauge");
        break;
      case METRIC_TYPE_UNTYPED:
        break;
      }
      luaC_pushcdtime(L, ptr->time);
      lua_setfield(L, -2, "time");

      luaC_pushcdtime(L, ptr->interval);
      lua_setfield(L, -2, "interval");

      if (ptr->meta) {
        char **toc;
        meta_data_t *meta = ptr->meta;
        int meta_count = meta_data_toc(meta, &toc);
        lua_newtable(L);
        for (int i = 0; i < meta_count; i++) {
          lua_newtable(L);

          int meta_type = meta_data_type(meta, toc[i]);
          int exist = meta_data_exists(meta, toc[i]);
          if (!exist) {
            DEBUG("lua: meta_data key not found: %s", toc[i]);
            continue;
          }
          lua_pushinteger(L, meta_type);
          lua_setfield(L, -2, "type");
          char *string;
          int64_t i64value;
          uint64_t u64value;
          double dvalue;
          bool bvalue;
          switch (meta_type) {
          case MD_TYPE_STRING:
            meta_data_get_string(meta, toc[i], &string);
            lua_pushstring(L, string);
            break;
          case MD_TYPE_SIGNED_INT:
            meta_data_get_signed_int(meta, toc[i], &i64value);
            lua_pushnumber(L, i64value);
            break;
          case MD_TYPE_UNSIGNED_INT:
            meta_data_get_unsigned_int(meta, toc[i], &u64value);
            lua_pushnumber(L, u64value);
            break;
          case MD_TYPE_DOUBLE:
            meta_data_get_double(meta, toc[i], &dvalue);
            lua_pushnumber(L, dvalue);
            break;
          case MD_TYPE_BOOLEAN:
            meta_data_get_boolean(meta, toc[i], &bvalue);
            lua_pushboolean(L, bvalue);
            break;
          default:
            break;
          }
          lua_setfield(L, -2, toc[i]);
          lua_rawseti(L, -2, i + 1);
        }
        if (meta_count > 0) {
          lua_setfield(L, -2, "meta");
          for (int i = 0; i < meta_count; i++) {
            free(toc[i]);
          }
        }
      }

      /* metric = { [N] = ...} */
      lua_rawseti(L, -2, i + 1);
      ptr++;
    }
    /* metric = {...} */
    lua_setfield(L, -2, "metric");
  }

  DEBUG("lua: luaC_pushmetricfamily successfully called.");

  return 0;
} /* }}} int luaC_pushmetricfamily */
