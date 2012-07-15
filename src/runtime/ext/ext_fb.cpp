/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   | Copyright (c) 1997-2010 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <runtime/ext/ext_fb.h>
#include <runtime/ext/ext_function.h>
#include <runtime/ext/ext_mysql.h>
#include <util/db_conn.h>
#include <util/logger.h>
#include <util/stat_cache.h>
#include <netinet/in.h>
#include <runtime/base/externals.h>
#include <runtime/base/string_util.h>
#include <runtime/base/util/string_buffer.h>
#include <runtime/base/code_coverage.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/array/zend_array.h>
#include <runtime/base/intercept.h>
#include <runtime/base/taint/taint_data.h>
#include <runtime/base/taint/taint_trace.h>
#include <runtime/base/taint/taint_warning.h>
#include <runtime/vm/backup_gc.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <util/parser/parser.h>

namespace HPHP {
IMPLEMENT_DEFAULT_EXTENSION(fb);
///////////////////////////////////////////////////////////////////////////////

static const UChar32 SUBSTITUTION_CHARACTER = 0xFFFD;

#define FB_UNSERIALIZE_NONSTRING_VALUE           0x0001
#define FB_UNSERIALIZE_UNEXPECTED_END            0x0002
#define FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE  0x0003
#define FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE 0x0004

const int64 k_FB_UNSERIALIZE_NONSTRING_VALUE = FB_UNSERIALIZE_NONSTRING_VALUE;
const int64 k_FB_UNSERIALIZE_UNEXPECTED_END = FB_UNSERIALIZE_UNEXPECTED_END;
const int64 k_FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE =
  FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE;
const int64 k_FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE =
  FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE;

const int64 k_TAINT_NONE = TAINT_BIT_NONE;
const int64 k_TAINT_HTML = TAINT_BIT_HTML;
const int64 k_TAINT_MUTATED = TAINT_BIT_MUTATED;
const int64 k_TAINT_SQL = TAINT_BIT_SQL;
const int64 k_TAINT_SHELL = TAINT_BIT_SHELL;
const int64 k_TAINT_TRACE_HTML = TAINT_BIT_TRACE_HTML;
const int64 k_TAINT_ALL = TAINT_BIT_ALL;
const int64 k_TAINT_TRACE_SELF = TAINT_BIT_TRACE_SELF;

///////////////////////////////////////////////////////////////////////////////

/* Linux and other systems don't currently support a ntohx or htonx
   set of functions for 64-bit values.  We've implemented our own here
   which is based off of GNU Net's implementation with some slight
   modifications (changed to macro's rather than functions). */
#if __BYTE_ORDER == __BIG_ENDIAN
#define ntohll(n) (n)
#define htonll(n) (n)
#else
#define ntohll(n) ( (((uint64_t)ntohl(n)) << 32) | ((uint64_t)ntohl(n >> 32) & 0x00000000ffffffff) )
#define htonll(n) ( (((uint64_t)htonl(n)) << 32) | ((uint64_t)htonl(n >> 32) & 0x00000000ffffffff) )
#endif

/* enum of thrift types */
enum TType {
  T_STOP    = 1,
  T_BYTE    = 2,
  T_U16     = 3,
  T_I16     = 4,
  T_U32     = 5,
  T_I32     = 6,
  T_U64     = 7,
  T_I64     = 8,
  T_STRING  = 9,
  T_STRUCT  = 10,
  T_MAP     = 11,
  T_SET     = 12,
  T_LIST    = 13,
  T_NULL    = 14,
  T_VARCHAR = 15,
  T_DOUBLE  = 16,
  T_BOOLEAN = 17,
};

/* Return the smallest size int that can store the value */
#define INT_SIZE(x) (((x) == ((int8_t)x))  ? 1 :    \
                     ((x) == ((int16_t)x)) ? 2 :    \
                     ((x) == ((int32_t)x)) ? 4 : 8)

/* Return the smallest (supported) unsigned length that can store the value */
#define LEN_SIZE(x) ((((unsigned)x) == ((uint8_t)x)) ? 1 : 4)

static int fb_serialized_size(CVarRef thing, int depth, int *bytes) {
  if (depth > 256) {
    return 1;
  }

  /* Get the size for an object, including one byte for the type */
  switch (thing.getType()) {
  case KindOfUninit:
  case KindOfNull:      *bytes = 1; break;     /* type */
  case KindOfBoolean:   *bytes = 2; break;    /* type + sizeof(char) */
  case KindOfInt64:     *bytes = 1 + INT_SIZE(thing.toInt64()); break;
  case KindOfDouble:    *bytes = 9; break;     /* type + sizeof(double) */
  case KindOfStaticString:
  case KindOfString:
    {
      int len = thing.toString().size();
      *bytes = 1 + LEN_SIZE(len) + len;
      break;
    }
  case KindOfArray:
    {
      int size = 2;
      Array arr = thing.toArray();
      for (ArrayIter iter(arr); iter; ++iter) {
        Variant key = iter.first();
        if (key.isNumeric()) {
          int64 index = key.toInt64();
          size += 1 + INT_SIZE(index);
        } else {
          int len = key.toString().size();
          size += 1 + LEN_SIZE(len) + len;
        }
        int additional_bytes = 0;
        if (fb_serialized_size(iter.second(), depth + 1,
                               &additional_bytes)) {
          return 1;
        }
        size += additional_bytes;
      }
      *bytes = size;
      break;
    }
  default:
    return 1;
  }
  return 0;
}

static void fb_serialize_long_into_buffer(int64 val, char *buff, int *pos) {
  switch (INT_SIZE(val)) {
  case 1:
    buff[(*pos)++] = T_BYTE;
    buff[(*pos)++] = (int8_t)val;
    break;
  case 2:
    buff[(*pos)++] = T_I16;
    *(int16_t *)(buff + (*pos)) = htons(val);
    (*pos) += 2;
    break;
  case 4:
    buff[(*pos)++] = T_I32;
    *(int32_t *)(buff + (*pos)) = htonl(val);
    (*pos) += 4;
    break;
  case 8:
    buff[(*pos)++] = T_I64;
    *(int64_t *)(buff + (*pos)) = htonll(val);
    (*pos) += 8;
    break;
  }
}

static void fb_serialize_string_into_buffer(CStrRef str, char *buf, int *pos) {
  int len = str.size();
  switch (LEN_SIZE(len)) {
  case 1:
    buf[(*pos)++] = T_VARCHAR;
    buf[(*pos)++] = (uint8_t)len;
    break;
  case 4:
    buf[(*pos)++] = T_STRING;
    *(uint32_t *)(buf + (*pos)) = htonl(len);
    (*pos) += 4;
    break;
  }

  /* memcpy the string into the buffer */
  memcpy(buf + (*pos), str.data(), len);
  (*pos) += len;
}

static bool fb_serialize_into_buffer(CVarRef thing, char *buff, int *pos) {
  switch (thing.getType()) {
  case KindOfNull:
    buff[(*pos)++] = T_NULL;
    break;
  case KindOfBoolean:
    buff[(*pos)++] = T_BOOLEAN;
    buff[(*pos)++] = (int8_t)thing.toInt64();
    break;
  case KindOfInt64:
    fb_serialize_long_into_buffer(thing.toInt64(), buff, pos);
    break;
  case KindOfDouble:
    buff[(*pos)++] = T_DOUBLE;
    *(double *)(buff + (*pos)) = thing.toDouble();
    (*pos) += 8;
    break;
  case KindOfStaticString:
  case KindOfString:
    fb_serialize_string_into_buffer(thing.toString(), buff, pos);
    break;
  case KindOfArray:
    {
      buff[(*pos)++] = T_STRUCT;
      Array arr = thing.toArray();
      for (ArrayIter iter(arr); iter; ++iter) {
        Variant key = iter.first();
        if (key.isNumeric()) {
          int64 index = key.toInt64();
          fb_serialize_long_into_buffer(index, buff, pos);
        } else {
          fb_serialize_string_into_buffer(key.toString(), buff, pos);
        }

        if (!fb_serialize_into_buffer(iter.second(), buff, pos)) {
          return false;
        }
      }

      /* Write the final stop marker */
      buff[(*pos)++] = T_STOP;
    }
    break;
  default:
    raise_warning("unserializable object unexpectedly passed through "
                  "fb_serialized_size");
    ASSERT(false);
  }
  return true;
}

/* Check if there are enough bytes left in the buffer */
#define CHECK_ENOUGH(bytes, pos, num) do {                  \
    if ((int)(bytes) > (int)((num) - (pos))) {              \
      return FB_UNSERIALIZE_UNEXPECTED_END;                 \
    }                                                       \
  } while (0)

int fb_unserialize_from_buffer(Variant &res, const char *buff,
                               int buff_len, int *pos) {

  /* Check we have at least 1 byte for the type */
  CHECK_ENOUGH(1, *pos, buff_len);

  int type;
  switch (type = buff[(*pos)++]) {
  case T_NULL:
    res = null;
    break;
  case T_BOOLEAN:
    CHECK_ENOUGH(sizeof(int8_t), *pos, buff_len);
    res = (bool)(int8_t)buff[(*pos)++];
    break;
  case T_BYTE:
    CHECK_ENOUGH(sizeof(int8_t), *pos, buff_len);
    res = (int8_t)buff[(*pos)++];
    break;
  case T_I16:
    {
      CHECK_ENOUGH(sizeof(int16_t), *pos, buff_len);
      int16_t ret = (int16_t)ntohs(*(int16_t *)(buff + (*pos)));
      (*pos) += 2;
      res = ret;
      break;
    }
  case T_I32:
    {
      CHECK_ENOUGH(sizeof(int32_t), *pos, buff_len);
      int32_t ret = (int32_t)ntohl(*(int32_t *)(buff + (*pos)));
      (*pos) += 4;
      res = ret;
      break;
    }
  case T_I64:
    {
      CHECK_ENOUGH(sizeof(int64_t), *pos, buff_len);
      int64_t ret = (int64_t)ntohll(*(int64_t *)(buff + (*pos)));
      (*pos) += 8;
      res = (int64)ret;
      break;
    }
  case T_DOUBLE:
    {
      CHECK_ENOUGH(sizeof(double), *pos, buff_len);
      double ret = *(double *)(buff + (*pos));
      (*pos) += 8;
      res = ret;
      break;
    }
  case T_VARCHAR:
    {
      CHECK_ENOUGH(sizeof(uint8_t), *pos, buff_len);
      int len = (uint8_t)buff[(*pos)++];

      CHECK_ENOUGH(len, *pos, buff_len);
      StringData* ret = NEW(StringData)(buff + (*pos), len, CopyString);
      (*pos) += len;
      res = ret;
      break;
    }
  case T_STRING:
    {
      CHECK_ENOUGH(sizeof(uint32_t), *pos, buff_len);
      int len = (uint32_t)ntohl(*(uint32_t *)(buff + (*pos)));
      (*pos) += 4;

      CHECK_ENOUGH(len, *pos, buff_len);
      StringData* ret = NEW(StringData)(buff + (*pos), len, CopyString);
      (*pos) += len;
      res = ret;
      break;
    }
  case T_STRUCT:
    {
      Array ret = Array::Create();
      /* Need at least 1 byte for type/stop */
      CHECK_ENOUGH(1, *pos, buff_len);
      while ((type = buff[(*pos)++]) != T_STOP) {
        String key;
        int64 index = 0;
        switch(type) {
        case T_BYTE:
          CHECK_ENOUGH(sizeof(int8_t), *pos, buff_len);
          index = (int8_t)buff[(*pos)++];
          break;
        case T_I16:
          {
            CHECK_ENOUGH(sizeof(int16_t), *pos, buff_len);
            index = (int16_t)ntohs(*(int16_t *)(buff + (*pos)));
            (*pos) += 2;
            break;
          }
        case T_I32:
          {
            CHECK_ENOUGH(sizeof(int32_t), *pos, buff_len);
            index = (int32_t)ntohl(*(int32_t *)(buff + (*pos)));
            (*pos) += 4;
            break;
          }
        case T_I64:
          {
            CHECK_ENOUGH(sizeof(int64_t), *pos, buff_len);
            index = (int64_t)ntohll(*(int64_t *)(buff + (*pos)));
            (*pos) += 8;
            break;
          }
        case T_VARCHAR:
          {
            CHECK_ENOUGH(sizeof(uint8_t), *pos, buff_len);
            int len = (uint8_t)buff[(*pos)++];

            CHECK_ENOUGH(len, *pos, buff_len);
            key = NEW(StringData)(buff + (*pos), len, CopyString);
            (*pos) += len;
            break;
          }
        case T_STRING:
          {
            CHECK_ENOUGH(sizeof(uint32_t), *pos, buff_len);
            int len = (uint32_t)ntohl(*(uint32_t *)(buff + (*pos)));
            (*pos) += 4;

            CHECK_ENOUGH(len, *pos, buff_len);
            key = NEW(StringData)(buff + (*pos), len, CopyString);
            (*pos) += len;
            break;
          }
        default:
          return FB_UNSERIALIZE_UNEXPECTED_ARRAY_KEY_TYPE;
        }

        Variant value;
        int retval;
        if ((retval = fb_unserialize_from_buffer(value, buff, buff_len, pos))) {
          return retval;
        }
        if (!key.isNull()) {
          ret.set(key, value);
        } else {
          ret.set(index, value);
        }
        /* Need at least 1 byte for type/stop (see start of loop) */
        CHECK_ENOUGH(1, *pos, buff_len);
      }
      res = ret;
    }
    break;
  default:
    return FB_UNSERIALIZE_UNRECOGNIZED_OBJECT_TYPE;
  }

  return 0;
}

Variant f_fb_thrift_serialize(CVarRef thing) {
  int len;
  if (fb_serialized_size(thing, 0, &len)) {
    return null;
  }
  char *buff = (char *)malloc(len + 1);
  int pos = 0;
  fb_serialize_into_buffer(thing, buff, &pos);

  ASSERT(pos == len);
  buff[len] = '\0';
  return String(buff, len, AttachString);
}

Variant f_fb_thrift_unserialize(CVarRef thing, VRefParam success,
                                VRefParam errcode /* = null_variant */) {
  int pos = 0;
  errcode = null;
  int errcd;
  Variant ret;
  success = false;
  if (thing.isString()) {
    String sthing = thing.toString();
    if ((errcd = fb_unserialize_from_buffer(ret, sthing.data(), sthing.size(),
                                            &pos))) {
      errcode = errcd;
    } else {
      success = true;
      return ret;
    }
  } else {
    errcode = FB_UNSERIALIZE_NONSTRING_VALUE;
  }
  return false;
}

Variant f_fb_serialize(CVarRef thing) {
  return f_fb_thrift_serialize(thing);
}

Variant f_fb_unserialize(CVarRef thing, VRefParam success,
                         VRefParam errcode /* = null_variant */) {
  return f_fb_thrift_unserialize(thing, ref(success), ref(errcode));
}

Variant f_fb_compact_serialize(CVarRef thing) {
  throw NotImplementedException(__func__);
}

Variant f_fb_compact_unserialize(CVarRef thing, VRefParam success,
                                 VRefParam errcode /* = null_variant */) {
  throw NotImplementedException(__func__);
}

///////////////////////////////////////////////////////////////////////////////

static void output_dataset(Array &ret, int affected, DBDataSet &ds,
                           const DBConn::ErrorInfoMap &errors) {
  ret.set("affected", affected);

  Array rows;
  MYSQL_FIELD *fields = ds.getFields();
  for (ds.moveFirst(); ds.getRow(); ds.moveNext()) {
    Array row;
    for (int i = 0; i < ds.getColCount(); i++) {
      const char *field = ds.getField(i);
      int len = ds.getFieldLength(i);
      row.set(String(fields[i].name, CopyString),
              mysql_makevalue(String(field, len, CopyString), fields + i));
    }
    rows.append(row);
  }
  ret.set("result", rows);

  if (!errors.empty()) {
    Array error, codes;
    for (DBConn::ErrorInfoMap::const_iterator iter = errors.begin();
         iter != errors.end(); ++iter) {
      error.set(iter->first, String(iter->second.msg));
      codes.set(iter->first, iter->second.code);
    }
    ret.set("error", error);
    ret.set("errno", codes);
  }
}

void f_fb_load_local_databases(CArrRef servers) {
  DBConn::ClearLocalDatabases();
  for (ArrayIter iter(servers); iter; ++iter) {
    int dbId = iter.first().toInt32();
    Array data = iter.second().toArray();
    if (!data.empty()) {
      std::vector< std::pair<string, string> > sessionVariables;
      if (data.exists("session_variable")) {
        Array sv = data["session_variable"].toArray();
        for (ArrayIter svIter(sv); svIter; ++svIter) {
          sessionVariables.push_back(std::pair<string, string>(
            svIter.first().toString().data(),
            svIter.second().toString().data()));
        }
      }
      DBConn::AddLocalDB(dbId, data["ip"].toString().data(),
                         data["db"].toString().data(),
                         data["port"].toInt32(),
                         data["username"].toString().data(),
                         data["password"].toString().data(),
                         sessionVariables);
    }
  }
}

Array f_fb_parallel_query(CArrRef sql_map, int max_thread /* = 50 */,
                          bool combine_result /* = true */,
                          bool retry_query_on_fail /* = true */,
                          int connect_timeout /* = -1 */,
                          int read_timeout /* = -1 */,
                          bool timeout_in_ms /* = false */) {
  if (!timeout_in_ms) {
    if (connect_timeout > 0) connect_timeout *= 1000;
    if (read_timeout > 0) read_timeout *= 1000;
  }

  ServerQueryVec queries;
  for (ArrayIter iter(sql_map); iter; ++iter) {
    Array data = iter.second().toArray();
    if (!data.empty()) {
      std::vector< std::pair<string, string> > sessionVariables;
      if (data.exists("session_variable")) {
        Array sv = data["session_variable"].toArray();
        for (ArrayIter svIter(sv); svIter; ++svIter) {
          sessionVariables.push_back(std::pair<string, string>(
            svIter.first().toString().data(),
            svIter.second().toString().data()));
        }
      }
      ServerDataPtr server
        (new ServerData(data["ip"].toString().data(),
                        data["db"].toString().data(),
                        data["port"].toInt32(),
                        data["username"].toString().data(),
                        data["password"].toString().data(),
                        sessionVariables));
      queries.push_back(ServerQuery(server, data["sql"].toString().data()));
    } else {
      // so we can report errors according to array index
      queries.push_back(ServerQuery(ServerDataPtr(), ""));
    }
  }

  Array ret;
  if (combine_result) {
    DBDataSet ds;
    DBConn::ErrorInfoMap errors;
    int affected = DBConn::parallelExecute(queries, ds, errors, max_thread,
                     retry_query_on_fail,
                     connect_timeout, read_timeout,
                     RuntimeOption::MySQLMaxRetryOpenOnFail,
                     RuntimeOption::MySQLMaxRetryQueryOnFail);
    output_dataset(ret, affected, ds, errors);
  } else {
    DBDataSetPtrVec dss(queries.size());
    for (unsigned int i = 0; i < dss.size(); i++) {
      dss[i] = DBDataSetPtr(new DBDataSet());
    }

    DBConn::ErrorInfoMap errors;
    int affected = DBConn::parallelExecute(queries, dss, errors, max_thread,
                     retry_query_on_fail,
                     connect_timeout, read_timeout,
                     RuntimeOption::MySQLMaxRetryOpenOnFail,
                     RuntimeOption::MySQLMaxRetryQueryOnFail);
    for (unsigned int i = 0; i < dss.size(); i++) {
      Array dsRet;
      output_dataset(dsRet, affected, *dss[i], errors);
      ret.append(dsRet);
    }
  }
  return ret;
}

Array f_fb_crossall_query(CStrRef sql, int max_thread /* = 50 */,
                          bool retry_query_on_fail /* = true */,
                          int connect_timeout /* = -1 */,
                          int read_timeout /* = -1 */,
                          bool timeout_in_ms /* = false */) {
  if (!timeout_in_ms) {
    if (connect_timeout > 0) connect_timeout *= 1000;
    if (read_timeout > 0) read_timeout *= 1000;
  }

  Array ret;
  // parameter checking
  if (!sql || !*sql) {
    ret.set("error", "empty SQL");
    return ret;
  }

  // security checking
  String ssql = StringUtil::ToLower(sql);
  if (ssql.find("where") < 0) {
    ret.set("error", "missing where clause");
    return ret;
  }
  if (ssql.find("select") < 0) {
    ret.set("error", "non-SELECT not supported");
    return ret;
  }

  // do it
  DBDataSet ds;
  DBConn::ErrorInfoMap errors;
  int affected = DBConn::parallelExecute(ssql.c_str(), ds, errors, max_thread,
                     retry_query_on_fail,
                     connect_timeout, read_timeout,
                     RuntimeOption::MySQLMaxRetryOpenOnFail,
                     RuntimeOption::MySQLMaxRetryQueryOnFail);
  output_dataset(ret, affected, ds, errors);
  return ret;
}

///////////////////////////////////////////////////////////////////////////////

bool f_fb_utf8ize(VRefParam input) {
  String s = input.toString();
  const char* const srcBuf = s.data();
  int32_t srcLenBytes = s.size();

  if (s.size() < 0 || s.size() > INT_MAX) {
    return false; // Too long.
  }

  // Preflight to avoid malloc() if the entire input is valid.
  int32_t srcPosBytes;
  for (srcPosBytes = 0; srcPosBytes < srcLenBytes; /* U8_NEXT increments */) {
    // This is lame, but gcc doesn't optimize U8_NEXT very well
    if (srcBuf[srcPosBytes] > 0 && srcBuf[srcPosBytes] <= 0x7f) {
      srcPosBytes++; // U8_NEXT would increment this
      continue;
    }
    UChar32 curCodePoint;
    // U8_NEXT() always advances srcPosBytes; save in case curCodePoint invalid
    int32_t savedSrcPosBytes = srcPosBytes;
    U8_NEXT(srcBuf, srcPosBytes, srcLenBytes, curCodePoint);
    if (curCodePoint <= 0) {
      // curCodePoint invalid; back up so we'll fix it in the loop below.
      srcPosBytes = savedSrcPosBytes;
      break;
    }
  }

  if (srcPosBytes == srcLenBytes) {
    // it's all valid
    return true;
  }

  // There are invalid bytes. Allocate memory, then copy the input, replacing
  // invalid sequences with either the substitution character or nothing,
  // depending on the value of RuntimeOption::Utf8izeReplace.
  //
  // Worst case, every remaining byte is invalid, taking a 3-byte substitution.
  int32_t bytesRemaining = srcLenBytes - srcPosBytes;
  uint64_t dstMaxLenBytes = srcPosBytes + (RuntimeOption::Utf8izeReplace ?
    bytesRemaining * U8_LENGTH(SUBSTITUTION_CHARACTER) :
    bytesRemaining);
  if (dstMaxLenBytes > INT_MAX) {
    return false; // Too long.
  }
  char *dstBuf = (char*)malloc(dstMaxLenBytes + 1);
  if (!dstBuf) {
    return false;
  }

  // Copy valid bytes found so far as one solid block.
  memcpy(dstBuf, srcBuf, srcPosBytes);

  // Iterate through the remaining bytes.
  int32_t dstPosBytes = srcPosBytes; // already copied srcPosBytes
  for (/* already init'd */; srcPosBytes < srcLenBytes; /* see U8_NEXT */) {
    UChar32 curCodePoint;
    // This is lame, but gcc doesn't optimize U8_NEXT very well
    if (srcBuf[srcPosBytes] > 0 && srcBuf[srcPosBytes] <= 0x7f) {
      curCodePoint = srcBuf[srcPosBytes++]; // U8_NEXT would increment
    } else {
      U8_NEXT(srcBuf, srcPosBytes, srcLenBytes, curCodePoint);
    }
    if (curCodePoint <= 0) {
      // Invalid UTF-8 sequence.
      // N.B. We consider a null byte an invalid sequence.
      if (!RuntimeOption::Utf8izeReplace) {
        continue; // Omit invalid sequence
      }
      curCodePoint = SUBSTITUTION_CHARACTER; // Replace invalid sequences
    }
    // We know that resultBuffer > total possible length.
    U8_APPEND_UNSAFE(dstBuf, dstPosBytes, curCodePoint);
  }
  dstBuf[dstPosBytes] = '\0';
  input = String(dstBuf, dstPosBytes, AttachString);
  return true;
}

/**
 * Private utf8_strlen implementation.
 *
 * Returns count of code points in input, substituting 1 code point per invalid
 * sequence.
 *
 * deprecated=true: instead return byte count on invalid UTF-8 sequence.
 */
static int f_fb_utf8_strlen_impl(CStrRef input, bool deprecated) {
  // Count, don't modify.
  int32_t sourceLength = input.size();
  const char* const sourceBuffer = input.data();
  int64_t num_code_points = 0;

  for (int32_t sourceOffset = 0; sourceOffset < sourceLength; ) {
    UChar32 sourceCodePoint;
    // U8_NEXT() is guaranteed to advance sourceOffset by 1-4 each time it's
    // invoked.
    U8_NEXT(sourceBuffer, sourceOffset, sourceLength, sourceCodePoint);
    if (deprecated && sourceCodePoint < 0) {
      return sourceLength; // return byte count on invalid sequence
    }
    num_code_points++;
  }
  return num_code_points;
}

int64 f_fb_utf8_strlen(CStrRef input) {
  return f_fb_utf8_strlen_impl(input, /* deprecated */ false);
}

int64 f_fb_utf8_strlen_deprecated(CStrRef input) {
  return f_fb_utf8_strlen_impl(input, /* deprecated */ true);
}

/**
 * Private helper; requires non-negative firstCodePoint and desiredCodePoints.
 */
static Variant f_fb_utf8_substr_simple(CStrRef str, int32_t firstCodePoint,
                                       int32_t numDesiredCodePoints) {
  const char* const srcBuf = str.data();
  char *dstBuf;
  int32_t srcLenBytes = str.size(); // May truncate; checked before use below.
  int32_t dstPosBytes = 0;

  ASSERT(firstCodePoint >= 0);  // Wrapper fixes up negative starting positions.
  ASSERT(numDesiredCodePoints > 0); // Wrapper fixes up negative/zero length.
  if (str.size() <= 0 ||
      str.size() > INT_MAX ||
      firstCodePoint >= srcLenBytes) {
    return false;
  }

  // Cannot be more code points than bytes in input.  This typically reduces
  // the INT_MAX default value to something more reasonable.
  numDesiredCodePoints = std::min(numDesiredCodePoints,
                                  srcLenBytes - firstCodePoint);

  // Pre-allocate the result.
  // the worst case can come from one of two sources:
  //  - every code point could be the substitution char (3 bytes)
  //    giving us numDesiredCodePoints * 3
  //  - every code point could be 4 bytes long, giving us
  //    numDesiredCodePoints * 4 - but capped by the length of the input
  uint64_t dstMaxLenBytes =
    std::min((uint64_t)numDesiredCodePoints * 4,
             (uint64_t)srcLenBytes - firstCodePoint);
  dstMaxLenBytes = std::max(dstMaxLenBytes,
                            (uint64_t)numDesiredCodePoints *
                            U8_LENGTH(SUBSTITUTION_CHARACTER));
  if (dstMaxLenBytes > INT_MAX) {
    return false; // Too long.
  }
  dstBuf = (char*)malloc(dstMaxLenBytes + 1);
  if (!dstBuf) {
    return false;
  }

  // Iterate through src's codepoints; srcPosBytes is incremented by U8_NEXT.
  for (int32_t srcPosBytes = 0, srcPosCodePoints = 0;
       srcPosBytes < srcLenBytes && // more available
       srcPosCodePoints < firstCodePoint + numDesiredCodePoints; // want more
       srcPosCodePoints++) {

    // U8_NEXT() advances sourceBytePos by 1-4 each time it's invoked.
    UChar32 curCodePoint;
    U8_NEXT(srcBuf, srcPosBytes, srcLenBytes, curCodePoint);

    if (srcPosCodePoints >= firstCodePoint) {
      // Copy this code point into the result.
      if (curCodePoint < 0) {
        curCodePoint = SUBSTITUTION_CHARACTER; // replace invalid sequences
      }
      // We know that resultBuffer > total possible length.
      // U8_APPEND_UNSAFE updates dstPosBytes.
      U8_APPEND_UNSAFE(dstBuf, dstPosBytes, curCodePoint);
    }
  }

  if (dstPosBytes > 0) {
    ASSERT(dstPosBytes <= (int32_t)dstMaxLenBytes);
    dstBuf[dstPosBytes] = '\0';
    return String(dstBuf, dstPosBytes, AttachString);
  }

  free(dstBuf);
  return false;
}

Variant f_fb_utf8_substr(CStrRef str, int start, int length /* = INT_MAX */) {
  // For negative start or length, calculate start and length values
  // based on total code points.
  if (start < 0 || length < 0) {
    // Get number of code points assuming we substitute invalid sequences.
    Variant utf8StrlenResult = f_fb_utf8_strlen(str);
    int32_t sourceNumCodePoints = utf8StrlenResult.toInt32();

    if (start < 0) {
      // Negative means first character is start'th code point from end.
      // e.g., -1 means start with the last code point.
      start = sourceNumCodePoints + start; // adding negative start
    }
    if (length < 0) {
      // Negative means omit last abs(length) code points.
      length = sourceNumCodePoints - start + length; // adding negative length
    }
  }

  if (start < 0 || length <= 0) {
    return false; // Empty result
  }

  return f_fb_utf8_substr_simple(str, start, length);
}

///////////////////////////////////////////////////////////////////////////////

bool f_fb_intercept(CStrRef name, CVarRef handler,
                    CVarRef data /* = null_variant */) {
  return register_intercept(name, handler, data);
}

Variant f_fb_stubout_intercept_handler(CStrRef name, CVarRef obj,
                                       CArrRef params, CVarRef data,
                                       VRefParam done) {
  if (obj.isNull()) {
    return f_call_user_func_array(data, params);
  }
  return f_call_user_func_array(CREATE_VECTOR2(obj, data), params);
}

Variant f_fb_rpc_intercept_handler(CStrRef name, CVarRef obj, CArrRef params,
                                   CVarRef data, VRefParam done) {
  String host = data["host"].toString();
  int port = data["port"].toInt32();
  String auth = data["auth"].toString();
  int timeout = data["timeout"].toInt32();

  if (obj.isNull()) {
    return f_call_user_func_array_rpc(host, port, auth, timeout, name, params);
  }
  return f_call_user_func_array_rpc(host, port, auth, timeout,
                                    CREATE_VECTOR2(obj, name), params);
}

void f_fb_renamed_functions(CArrRef names) {
  check_renamed_functions(names);
}

bool f_fb_rename_function(CStrRef orig_func_name, CStrRef new_func_name) {
  if (orig_func_name.empty() || new_func_name.empty() ||
      orig_func_name->isame(new_func_name.get())) {
    throw_invalid_argument("unable to rename %s", orig_func_name.data());
    return false;
  }

  if (!function_exists(orig_func_name)) {
    raise_warning("fb_rename_function(%s, %s) failed: %s does not exists!",
                  orig_func_name.data(), new_func_name.data(),
                  orig_func_name.data());
    return false;
  }

  if (function_exists(new_func_name)) {
    if (new_func_name.data()[0] !=
        ParserBase::CharCreateFunction) { // create_function
      raise_warning("fb_rename_function(%s, %s) failed: %s already exists!",
                    orig_func_name.data(), new_func_name.data(),
                    new_func_name.data());
      return false;
    }
  }

  if (!check_renamed_function(orig_func_name) &&
      !check_renamed_function(new_func_name)) {
    raise_error("fb_rename_function(%s, %s) failed: %s is not allowed to "
                "rename. Please add it to the list provided to "
                "fb_renamed_functions().",
                orig_func_name.data(), new_func_name.data(),
                orig_func_name.data());
    return false;
  }

  rename_function(orig_func_name, new_func_name);
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// call_user_func extensions

Array f_fb_call_user_func_safe(int _argc, CVarRef function,
                               CArrRef _argv /* = null_array */) {
  return f_fb_call_user_func_array_safe(function, _argv);
}

static Variant fb_call_user_func_safe(CVarRef function, CArrRef params,
                                      bool &ok) {
  MethodCallPackage mcp;
  String classname, methodname;
  bool doBind;
  if (get_callable_user_func_handler(function,
                                     mcp, classname, methodname, doBind)) {
    ok = true;
    if (doBind) {
      FrameInjection::StaticClassNameHelper scn(
        ThreadInfo::s_threadInfo.getNoCheck(), classname);
      ASSERT(!mcp.m_isFunc);
      return mcp.ci->getMeth()(mcp, params);
    } else {
      if (mcp.m_isFunc) {
        return mcp.ci->getFunc()(mcp.extra, params);
      } else {
        return mcp.ci->getMeth()(mcp, params);
      }
    }
  }
  ok = false;
  return null;
}

Variant f_fb_call_user_func_safe_return(int _argc, CVarRef function,
                                        CVarRef def,
                                        CArrRef _argv /* = null_array */) {
  if (hhvm) {
    if (f_is_callable(function)) {
      return f_call_user_func_array(function, _argv);
    }
    return def;
  } else {
    bool ok;
    Variant ret = fb_call_user_func_safe(function, _argv, ok);
    return ok ? ret : def;
  }
}

Array f_fb_call_user_func_array_safe(CVarRef function, CArrRef params) {
  if (hhvm) {
    if (f_is_callable(function)) {
      return CREATE_VECTOR2(true, f_call_user_func_array(function, params));
    }
    return CREATE_VECTOR2(false, null);
  } else {
    bool ok;
    Variant ret = fb_call_user_func_safe(function, params, ok);
    return CREATE_VECTOR2(ok, ret);
  }
}

///////////////////////////////////////////////////////////////////////////////

Variant f_fb_get_code_coverage(bool flush) {
  ThreadInfo *ti = ThreadInfo::s_threadInfo.getNoCheck();
  if (ti->m_reqInjectionData.coverage) {
    Array ret = ti->m_coverage->Report();
    if (flush) {
      ti->m_coverage->Reset();
    }
    return ret;
  }
  return false;
}

void f_fb_enable_code_coverage() {
  ThreadInfo *ti = ThreadInfo::s_threadInfo.getNoCheck();
  ti->m_coverage->Reset();
  ti->m_reqInjectionData.coverage = true;
  if (hhvm) {
    if (g_vmContext->isNested()) {
      raise_notice("Calling fb_enable_code_coverage from a nested "
                   "VM instance may cause unpredicable results");
    }
    throw VMSwitchModeException(true);
  }
}

Variant f_fb_disable_code_coverage() {
  ThreadInfo *ti = ThreadInfo::s_threadInfo.getNoCheck();
  ti->m_reqInjectionData.coverage = false;
  Array ret = ti->m_coverage->Report();
  ti->m_coverage->Reset();
  return ret;
}

///////////////////////////////////////////////////////////////////////////////

void f_fb_set_taint(VRefParam str, int taint) {
#ifdef TAINTED
  if (!str.isString()) {
    return;
  }

  StringData *sd = str.getStringData();
  ASSERT(sd);
  if (sd->getCount() > 1) {
    // Pass taint to our copy.
    TAINT_OBSERVER(TAINT_BIT_NONE, TAINT_BIT_NONE);
    str = NEW(StringData)(sd->data(), sd->size(), CopyString);
  }

  str.getStringData()->getTaintDataRef().setTaint(taint);
#endif
}

void f_fb_unset_taint(VRefParam str, int taint) {
#ifdef TAINTED
  if (!str.isString()) {
    return;
  }

  StringData *sd = str.getStringData();
  ASSERT(sd);
  if (sd->getCount() > 1) {
    // Pass taint to our copy.
    TAINT_OBSERVER(TAINT_BIT_NONE, TAINT_BIT_NONE);
    str = NEW(StringData)(sd->data(), sd->size(), CopyString);
  }

  str.getStringData()->getTaintDataRef().unsetTaint(taint);
#endif
}

bool f_fb_get_taint(CStrRef str, int taint) {
#ifdef TAINTED
  StringData *string_data = str.get();
  ASSERT(string_data);
  return string_data->getTaintDataRefConst().getTaint() & taint;
#else
  return false;
#endif
}

Array f_fb_get_taint_warning_counts() {
#ifdef TAINTED
  return TaintWarning::GetCounts();
#else
  Array counts;
  counts.set(TAINT_BIT_HTML, 0);
  counts.set(TAINT_BIT_MUTATED, 0);
  counts.set(TAINT_BIT_SQL, 0);
  counts.set(TAINT_BIT_SHELL, 0);
  counts.set(TAINT_BIT_ALL, 0);
  return counts;
#endif
}

void f_fb_enable_html_taint_trace() {
#ifdef TAINTED
  TaintTracer::SwitchTrace(TAINT_BIT_TRACE_HTML, true);
#endif
}

bool f_fb_output_compression(bool new_value) {
  Transport *transport = g_context->getTransport();
  if (transport) {
    bool rv = transport->isCompressionEnabled();
    if (new_value) {
      transport->enableCompression();
    } else {
      transport->disableCompression();
    }
    return rv;
  }
  return false;
}

void f_fb_set_exit_callback(CVarRef function) {
  g_context->setExitCallback(function);
}

Array f_fb_get_flush_stat() {
  Transport *transport = g_context->getTransport();
  if (transport) {
    Array chunkStats(ArrayData::Create());
    transport->getChunkSentSizes(chunkStats);

    int total = transport->getResponseTotalSize();
    int sent = transport->getResponseSentSize();
    int64 time = transport->getFlushTime();
    return CREATE_MAP2(
        "flush_stats", CREATE_MAP3("total", total, "sent", sent, "time", time),
        "chunk_stats", chunkStats);
  }
  return NULL;
}

int64 f_fb_get_last_flush_size() {
  Transport *transport = g_context->getTransport();
  return transport ? transport->getLastChunkSentSize() : 0;
}

extern Array stat_impl(struct stat*); // ext_file.cpp

template<class Function>
static Variant do_lazy_stat(Function dostat, CStrRef filename) {
  struct stat sb;
  if (dostat(File::TranslatePath(filename, true).c_str(), &sb)) {
    Logger::Verbose("%s/%d: %s", __FUNCTION__, __LINE__,
                    Util::safe_strerror(errno).c_str());
    return false;
  }
  return stat_impl(&sb);
}

Variant f_fb_lazy_stat(CStrRef filename) {
  return do_lazy_stat(StatCache::stat, filename);
}

Variant f_fb_lazy_lstat(CStrRef filename) {
  return do_lazy_stat(StatCache::lstat, filename);
}

String f_fb_lazy_realpath(CStrRef filename) {
  return StatCache::realpath(filename.c_str());
}

String f_fb_gc_collect_cycles() {
  std::string s = VM::gc_collect_cycles();
  return String(s);
}

void f_fb_gc_detect_cycles(CStrRef filename) {
  VM::gc_detect_cycles(std::string(filename.c_str()));
}

///////////////////////////////////////////////////////////////////////////////
// const index functions

static Array const_data;

KEEP_SECTION
Variant f_fb_const_fetch(CVarRef key) {
  String k = key.toString();
  Variant *ret = const_data.lvalPtr(k, false, false);
  if (ret) return *ret;
  return false;
}

void const_load_set(CStrRef key, CVarRef value) {
  const_data.set(key, value, true);
}

KEEP_SECTION
void const_load() {
  // after all loading
  const_load_set("zend_array_size", const_data.size());
  const_data.setEvalScalar();
}

bool const_dump(const char *filename) {
  std::ofstream out(filename);
  if (out.fail()) {
    return false;
  }
  const_data->dump(out);
  out.close();
  return true;
}

///////////////////////////////////////////////////////////////////////////////
}
