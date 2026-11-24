/**
  tLuaCOMTypeHandler.cpp: Class tLuaCOMTypeHandler, which handles
  conversion of values between Lua and COM values.
*/

// Required for VARIANT field accessors to compile under wineg++.
#ifdef __WINE__
# define _FORCENAMELESSUNION  // used by oaidl.h
#endif

#include <ole2.h>

extern "C"
{
#include <lua.h>
}
#include "LuaCompat.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <vector>
#include <exception>

#include "tLuaCOMTypeHandler.h"
#include "tLuaCOM.h"
#include "tLuaVector.h"
#include "tLuaCOMException.h"
#include "tLuaDispatch.h"
#include "tCOMUtil.h"

#include "tLuaCOMEnumerator.h"

#include "tUtil.h"
#include "LuaAux.h"
#include "luabeans.h"

#include "luacom_internal.h"

#define LUA_NOOBJECT 0

static const VARTYPE com_types[] = {
  VT_R8, VT_R4, VT_ARRAY, VT_R8, VT_R8, VT_I8, VT_UI8, VT_I4, VT_UI4,
  VT_I2, VT_UI2, VT_I1, VT_UI1, VT_INT, VT_UINT, VT_BSTR, VT_NULL, VT_ERROR, VT_BOOL
};

static const char *const com_type_names[] = {
  "double", "float", "array", "decimal", "currency", "int8", "uint8", "int4", "uint4",
  "int2", "uint2", "int1", "uint1", "int", "uint", "string", "null", "error", "bool", NULL
};

namespace
{
// Calls that can run Lua code must fail through C++ exceptions here, so the
// surrounding VARIANT, SAFEARRAY and string owners are unwound normally.
void callLuaFunction(lua_State* L, int arguments, int results)
{
  tStringBuffer error;
  if(luaCompat_call(L, arguments, results, error) != 0)
    TYPECONV_ERROR(error.getBuffer() ? error.getBuffer() : "Lua conversion failed");
}

void getLuaTableValue(lua_State* L, int index)
{
  if(index < 0 && index > LUA_REGISTRYINDEX)
    index += lua_gettop(L) + 1;
  if(!lua_checkstack(L, 3))
    TYPECONV_ERROR("Insufficient Lua stack space for table lookup");

  lua_pushcfunction(L, LuaAux::getTableValue);
  lua_pushvalue(L, index);
  lua_pushvalue(L, -3); // original key, below the function and table
  callLuaFunction(L, 2, 1);
  lua_replace(L, -2); // replace the original key with the result
}


class VariantValue
{
public:
  VariantValue() { VariantInit(&value); }
  ~VariantValue() { VariantClear(&value); }

  void detach() { VariantInit(&value); }

  VARIANT value;
};

class LuaStackGuard
{
public:
  explicit LuaStackGuard(lua_State* state)
    : L(state), top(lua_gettop(state)), active(true) {}
  ~LuaStackGuard()
  {
    if(active)
      lua_settop(L, top);
  }

  void dismiss() { active = false; }

private:
  lua_State* L;
  int top;
  bool active;
};

const char* numericVariantTypeName(VARTYPE vt)
{
  switch(vt)
  {
    case VT_CY: return "currency";
    case VT_UI1: return "uint1";
    case VT_UI2: return "uint2";
    case VT_UI4: return "uint4";
    case VT_UI8: return "uint8";
    case VT_INT: return "int";
    case VT_UINT: return "uint";
    case VT_I1: return "int1";
    case VT_I2: return "int2";
    case VT_I4: return "int4";
    case VT_I8: return "int8";
    case VT_R4: return "float";
    case VT_DECIMAL: return "decimal";
    case VT_R8: return "double";
    default: return "double";
  }
}

void pushTableVariantHeader(lua_State* L, VARTYPE vt)
{
  lua_newtable(L);
  lua_pushstring(L, "Type");
  lua_pushstring(L, numericVariantTypeName(vt));
  lua_settable(L, -3);
  lua_pushstring(L, "Value");
}

bool parseUnsignedDecimal(const char* text, size_t length, ULONGLONG limit,
                          ULONGLONG* result)
{
  if(!text || length == 0)
    return false;

  ULONGLONG value = 0;
  for(size_t i = 0; i < length; i++)
  {
    if(text[i] < '0' || text[i] > '9')
      return false;

    const ULONGLONG digit = static_cast<ULONGLONG>(text[i] - '0');
    if(value > (limit - digit) / 10)
      return false;
    value = value * 10 + digit;
  }

  *result = value;
  return true;
}

bool parseSignedDecimal(const char* text, size_t length, LONGLONG* result)
{
  if(!text || length == 0)
    return false;

  const bool negative = text[0] == '-';
  const size_t first_digit = negative ? 1 : 0;
  if(first_digit == length)
    return false;

  const ULONGLONG positive_limit = 9223372036854775807ULL;
  const ULONGLONG negative_limit = 9223372036854775808ULL;
  ULONGLONG magnitude = 0;
  if(!parseUnsignedDecimal(text + first_digit, length - first_digit,
                           negative ? negative_limit : positive_limit,
                           &magnitude))
    return false;

  if(negative)
  {
    if(magnitude == negative_limit)
      *result = -9223372036854775807LL - 1;
    else
      *result = -static_cast<LONGLONG>(magnitude);
  }
  else
    *result = static_cast<LONGLONG>(magnitude);

  return true;
}

bool luaNumberToSignedInteger(lua_Number number, LONGLONG* result)
{
  const lua_Number exact_limit = static_cast<lua_Number>(9007199254740992.0);
  if(number != number || number < -exact_limit || number > exact_limit)
    return false;

  const LONGLONG value = static_cast<LONGLONG>(number);
  if(static_cast<lua_Number>(value) != number)
    return false;

  *result = value;
  return true;
}

bool luaNumberToUnsignedInteger(lua_Number number, ULONGLONG* result)
{
  const lua_Number exact_limit = static_cast<lua_Number>(9007199254740992.0);
  if(number != number || number < 0 || number > exact_limit)
    return false;

  const ULONGLONG value = static_cast<ULONGLONG>(number);
  if(static_cast<lua_Number>(value) != number)
    return false;

  *result = value;
  return true;
}

bool luaValueToSignedInteger(lua_State* L, int index, LONGLONG* result)
{
#if LUA_VERSION_NUM >= 503
  if(lua_isinteger(L, index))
  {
    *result = static_cast<LONGLONG>(lua_tointeger(L, index));
    return true;
  }
#endif
  return lua_type(L, index) == LUA_TNUMBER &&
         luaNumberToSignedInteger(lua_tonumber(L, index), result);
}

bool luaValueToUnsignedInteger(lua_State* L, int index, ULONGLONG* result)
{
#if LUA_VERSION_NUM >= 503
  if(lua_isinteger(L, index))
  {
    const lua_Integer value = lua_tointeger(L, index);
    if(value < 0)
      return false;
    *result = static_cast<ULONGLONG>(value);
    return true;
  }
#endif
  return lua_type(L, index) == LUA_TNUMBER &&
         luaNumberToUnsignedInteger(lua_tonumber(L, index), result);
}

bool luaValueToSignedIntegerOrString(lua_State* L, int index, LONGLONG* result)
{
  if(lua_type(L, index) == LUA_TSTRING)
  {
    size_t length = 0;
    const char* value = lua_tolstring(L, index, &length);
    return parseSignedDecimal(value, length, result);
  }
  return luaValueToSignedInteger(L, index, result);
}

bool luaValueToUnsignedIntegerOrString(lua_State* L, int index,
                                       ULONGLONG* result)
{
  if(lua_type(L, index) == LUA_TSTRING)
  {
    size_t length = 0;
    const char* value = lua_tolstring(L, index, &length);
    return parseUnsignedDecimal(value, length,
                                ~static_cast<ULONGLONG>(0), result);
  }
  return luaValueToUnsignedInteger(L, index, result);
}

void pushUnsignedIntegerString(lua_State* L, ULONGLONG value, bool negative)
{
  char buffer[32];
  char* end = buffer + sizeof(buffer);
  char* current = end;

  do
  {
    *--current = static_cast<char>('0' + value % 10);
    value /= 10;
  }
  while(value != 0);

  if(negative)
    *--current = '-';

  lua_pushlstring(L, current, static_cast<size_t>(end - current));
}

void pushSignedInteger(lua_State* L, LONGLONG value)
{
#if LUA_VERSION_NUM >= 503
  if(value >= static_cast<LONGLONG>(LUA_MININTEGER) &&
     value <= static_cast<LONGLONG>(LUA_MAXINTEGER))
  {
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    return;
  }
#endif
  if(value >= -9007199254740992LL && value <= 9007199254740992LL)
  {
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return;
  }

  const bool negative = value < 0;
  const ULONGLONG magnitude = negative
    ? static_cast<ULONGLONG>(-(value + 1)) + 1
    : static_cast<ULONGLONG>(value);
  pushUnsignedIntegerString(L, magnitude, negative);
}

void pushUnsignedInteger(lua_State* L, ULONGLONG value)
{
#if LUA_VERSION_NUM >= 503
  if(value <= static_cast<ULONGLONG>(LUA_MAXINTEGER))
  {
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    return;
  }
#endif
  if(value <= 9007199254740992ULL)
  {
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return;
  }

  pushUnsignedIntegerString(L, value, false);
}

// DECIMAL and CURRENCY values as exact text, independent of the locale.
// A magnitude holds 96 bits, the most significant word first.
const unsigned MAX_DECIMAL_SCALE = 28;
const unsigned CURRENCY_SCALE = 4;

// magnitude = magnitude * 10 + digit; false if 96 bits do not suffice
bool multiplyMagnitudeAdd(ULONG magnitude[3], unsigned digit)
{
  ULONGLONG carry = digit;
  for(int i = 2; i >= 0; i--)
  {
    const ULONGLONG part = static_cast<ULONGLONG>(magnitude[i]) * 10 + carry;
    magnitude[i] = static_cast<ULONG>(part & 0xFFFFFFFFUL);
    carry = part >> 32;
  }
  return carry == 0;
}

// magnitude = magnitude / 10; returns the remainder
unsigned divideMagnitudeBy10(ULONG magnitude[3])
{
  ULONGLONG remainder = 0;
  for(int i = 0; i < 3; i++)
  {
    const ULONGLONG part = (remainder << 32) | magnitude[i];
    magnitude[i] = static_cast<ULONG>(part / 10);
    remainder = part % 10;
  }
  return static_cast<unsigned>(remainder);
}

// Parses [+|-]digits[.digits] with at most max_scale decimals.
bool parseScaledDecimal(const char* text, size_t length, unsigned max_scale,
                        ULONG magnitude[3], bool* negative, unsigned* scale)
{
  if(!text || length == 0)
    return false;

  size_t i = 0;
  *negative = false;
  if(text[0] == '-' || text[0] == '+')
  {
    *negative = text[0] == '-';
    i = 1;
  }

  magnitude[0] = magnitude[1] = magnitude[2] = 0;
  size_t integer_digits = 0;
  unsigned decimals = 0;
  bool point = false;
  for(; i < length; i++)
  {
    if(text[i] == '.' && !point && integer_digits > 0)
    {
      point = true;
      continue;
    }
    if(text[i] < '0' || text[i] > '9')
      return false;
    if(point)
    {
      if(decimals == max_scale)
        return false;
      decimals++;
    }
    else
      integer_digits++;
    if(!multiplyMagnitudeAdd(magnitude, static_cast<unsigned>(text[i] - '0')))
      return false;
  }

  if(integer_digits == 0 || (point && decimals == 0))
    return false;
  *scale = decimals;
  return true;
}

// Writes magnitude / 10^scale as text; the buffer needs 32 characters.
size_t formatScaledDecimal(const ULONG magnitude[3], bool negative,
                           unsigned scale, char* text)
{
  ULONG rest[3] = {magnitude[0], magnitude[1], magnitude[2]};
  char digits[32];
  size_t count = 0;
  bool zero = true;
  do
  {
    const unsigned digit = divideMagnitudeBy10(rest);
    zero = zero && digit == 0;
    digits[count++] = static_cast<char>('0' + digit);
  }
  while(rest[0] != 0 || rest[1] != 0 || rest[2] != 0);

  // one digit before the decimal point
  while(count <= scale)
    digits[count++] = '0';

  size_t length = 0;
  if(negative && !zero)
    text[length++] = '-';
  for(size_t i = count; i > 0; i--)
  {
    if(i == scale)
      text[length++] = '.';
    text[length++] = digits[i - 1];
  }
  return length;
}

void signedToMagnitude(LONGLONG value, ULONG magnitude[3], bool* negative)
{
  *negative = value < 0;
  const ULONGLONG units = *negative
    ? static_cast<ULONGLONG>(-(value + 1)) + 1
    : static_cast<ULONGLONG>(value);
  magnitude[0] = 0;
  magnitude[1] = static_cast<ULONG>(units >> 32);
  magnitude[2] = static_cast<ULONG>(units & 0xFFFFFFFFUL);
}

// Converts a magnitude of at most 64 bits to a signed value.
bool magnitudeToSigned(const ULONG magnitude[3], bool negative,
                       LONGLONG* value)
{
  if(magnitude[0] != 0)
    return false;
  const ULONGLONG units =
    (static_cast<ULONGLONG>(magnitude[1]) << 32) | magnitude[2];
  if(negative)
  {
    if(units > 9223372036854775808ULL)
      return false;
    *value = units == 9223372036854775808ULL
      ? -9223372036854775807LL - 1
      : -static_cast<LONGLONG>(units);
  }
  else
  {
    if(units > 9223372036854775807ULL)
      return false;
    *value = static_cast<LONGLONG>(units);
  }
  return true;
}
// Pushes a VT_DECIMAL or VT_CY value exactly, as text. With integers, a
// DECIMAL of scale 0 becomes a Lua integer where it fits, as VT_I8 does.
void pushExactDecimal(lua_State* L, const VARIANT& value, bool integers)
{
  ULONG magnitude[3];
  bool negative = false;
  unsigned scale = CURRENCY_SCALE;
  if(value.vt == VT_CY)
    signedToMagnitude(value.cyVal.int64, magnitude, &negative);
  else
  {
    magnitude[0] = value.decVal.Hi32;
    magnitude[1] = value.decVal.Mid32;
    magnitude[2] = value.decVal.Lo32;
    negative = (value.decVal.sign & DECIMAL_NEG) != 0;
    scale = value.decVal.scale;
    if(scale > MAX_DECIMAL_SCALE)
      TYPECONV_ERROR("DECIMAL value with an invalid scale.");

    LONGLONG integer = 0;
    if(integers && scale == 0 &&
       magnitudeToSigned(magnitude, negative, &integer))
    {
      pushSignedInteger(L, integer);
      return;
    }
  }

  char text[32];
  const size_t length = formatScaledDecimal(magnitude, negative, scale, text);
  lua_pushlstring(L, text, length);
}

void setDecimalVariant(VARIANTARG& varg, const ULONG magnitude[3],
                       bool negative, unsigned scale)
{
  const bool zero = magnitude[0] == 0 && magnitude[1] == 0 &&
                    magnitude[2] == 0;
  varg.decVal.wReserved = 0;
  varg.decVal.Hi32 = magnitude[0];
  varg.decVal.Mid32 = magnitude[1];
  varg.decVal.Lo32 = magnitude[2];
  varg.decVal.scale = static_cast<BYTE>(scale);
  varg.decVal.sign = negative && !zero ? DECIMAL_NEG : 0;
  // wReserved shares its storage with vt, so vt is set last.
  varg.vt = VT_DECIMAL;
}

// {Type="decimal"}: strings and Lua integers are converted exactly, other
// numbers through a double.
void luaValueToDecimal(lua_State* L, int index, VARIANTARG& varg)
{
  ULONG magnitude[3];
  bool negative = false;
  unsigned scale = 0;
  if(lua_type(L, index) == LUA_TSTRING)
  {
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    if(!parseScaledDecimal(text, length, MAX_DECIMAL_SCALE, magnitude,
                           &negative, &scale))
      TYPECONV_ERROR("decimal strings must contain one decimal number in range with at most 28 decimals.");
    setDecimalVariant(varg, magnitude, negative, scale);
    return;
  }
#if LUA_VERSION_NUM >= 503
  if(lua_isinteger(L, index))
  {
    signedToMagnitude(static_cast<LONGLONG>(lua_tointeger(L, index)),
                      magnitude, &negative);
    setDecimalVariant(varg, magnitude, negative, 0);
    return;
  }
#endif
  varg.vt = VT_R8;
  varg.dblVal = lua_tonumber(L, index);
  HRESULT hr = VariantChangeType(&varg, &varg, 0, VT_DECIMAL);
  CHK_COM_CODE(hr);
}

// {Type="currency"}: as decimal, with at most 4 decimals.
void luaValueToCurrency(lua_State* L, int index, VARIANTARG& varg)
{
  LONGLONG units = 0;
  if(lua_type(L, index) == LUA_TSTRING)
  {
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    ULONG magnitude[3];
    bool negative = false;
    unsigned scale = 0;
    bool valid = parseScaledDecimal(text, length, CURRENCY_SCALE, magnitude,
                                    &negative, &scale);
    for(; valid && scale < CURRENCY_SCALE; scale++)
      valid = multiplyMagnitudeAdd(magnitude, 0);
    if(!valid || !magnitudeToSigned(magnitude, negative, &units))
      TYPECONV_ERROR("currency strings must contain one decimal number in range with at most 4 decimals.");
    varg.vt = VT_CY;
    varg.cyVal.int64 = units;
    return;
  }
#if LUA_VERSION_NUM >= 503
  if(lua_isinteger(L, index))
  {
    const lua_Integer value = lua_tointeger(L, index);
    if(value < -922337203685477LL || value > 922337203685477LL)
      TYPECONV_ERROR("currency values must be in range.");
    varg.vt = VT_CY;
    varg.cyVal.int64 = static_cast<LONGLONG>(value) * 10000;
    return;
  }
#endif
  varg.vt = VT_R8;
  varg.dblVal = lua_tonumber(L, index);
  HRESULT hr = VariantChangeType(&varg, &varg, 0, VT_CY);
  CHK_COM_CODE(hr);
}

// Values of the integer types other than VT_I8 and VT_UI8.
LONGLONG signedVariantValue(const VARIANT& value)
{
  switch(value.vt)
  {
    case VT_I1: return static_cast<signed char>(value.cVal);
    case VT_I2: return value.iVal;
    case VT_I4: return value.lVal;
    default: return value.intVal;
  }
}

ULONGLONG unsignedVariantValue(const VARIANT& value)
{
  switch(value.vt)
  {
    case VT_UI1: return value.bVal;
    case VT_UI2: return value.uiVal;
    case VT_UI4: return value.ulVal;
    default: return value.uintVal;
  }
}

// Reads a format field of the luacom table; nil gives default_format.
int readGlobalFormat(lua_State* L, const char* name,
                     bool (*parse)(const char*, int*), int default_format)
{
  lua_getglobal(L, "luacom");
  lua_pushstring(L, name);
  getLuaTableValue(L, -2);
  int format = default_format;
  if(!lua_isnil(L, -1))
  {
    char message[96];
    if(lua_type(L, -1) != LUA_TSTRING)
    {
      snprintf(message, sizeof(message), "%s must be a string.", name);
      TYPECONV_ERROR(message);
    }
    if(!parse(lua_tostring(L, -1), &format))
    {
      snprintf(message, sizeof(message), "Unknown %s value '%.40s'.", name,
               lua_tostring(L, -1));
      TYPECONV_ERROR(message);
    }
  }
  lua_pop(L, 2);
  return format;
}

bool pushLuaDispatchIfSameState(lua_State* L, IUnknown* object)
{
  tCOMPtr<IUnknown> proxy_manager;
  HRESULT hr = object->QueryInterface(IID_IProxyManager, (void**)&proxy_manager);
  if(hr != E_NOINTERFACE)
    return false;

  tCOMPtr<ILuaDispatch> lua_dispatch;
  hr = object->QueryInterface(IID_ILuaDispatch, (void**)&lua_dispatch);
  return SUCCEEDED(hr) && SUCCEEDED(lua_dispatch->PushIfSameState(L));
}

}


tLuaCOMTypeHandler::tLuaCOMTypeHandler(ITypeInfo *ptypeinfo)
  : date_format(CONVERSION_GLOBAL), decimal_format(CONVERSION_GLOBAL)
{
  m_typeinfo.Attach(ptypeinfo);
  if(m_typeinfo) m_typeinfo->AddRef();
}

tLuaCOMTypeHandler::~tLuaCOMTypeHandler()
{
}


bool tLuaCOMTypeHandler::parseDateFormat(const char* name, int* format)
{
  if(strcmp(name, "string") == 0)
    *format = DATE_FORMAT_STRING;
  else if(strcmp(name, "table") == 0)
    *format = DATE_FORMAT_TABLE;
  else if(strcmp(name, "string_ms_accurate") == 0)
    *format = DATE_FORMAT_STRING_MS_ACCURATE;
  else if(strcmp(name, "iso") == 0)
    *format = DATE_FORMAT_ISO;
  else
    return false;
  return true;
}

bool tLuaCOMTypeHandler::parseDecimalFormat(const char* name, int* format)
{
  if(strcmp(name, "number") == 0)
    *format = DECIMAL_FORMAT_NUMBER;
  else if(strcmp(name, "string") == 0)
    *format = DECIMAL_FORMAT_STRING;
  else if(strcmp(name, "exact") == 0)
    *format = DECIMAL_FORMAT_EXACT;
  else
    return false;
  return true;
}

/**
  Sets the conversion of DATE, DECIMAL and CURRENCY values for this
  handler. CONVERSION_GLOBAL follows the field of the luacom table.
*/
void tLuaCOMTypeHandler::setConversion(int dates, int decimals)
{
  date_format = dates;
  decimal_format = decimals;
}

int tLuaCOMTypeHandler::dateFormat(lua_State* L)
{
  if(date_format != CONVERSION_GLOBAL)
    return date_format;
  return readGlobalFormat(L, "DateFormat", parseDateFormat,
                          DATE_FORMAT_STRING);
}

int tLuaCOMTypeHandler::decimalFormat(lua_State* L)
{
  if(decimal_format != CONVERSION_GLOBAL)
    return decimal_format;
  return readGlobalFormat(L, "DecimalFormat", parseDecimalFormat,
                          DECIMAL_FORMAT_NUMBER);
}


/**
  Pushes onto Lua stack table explicitly representing VARIANT of type `vt` and
  numeric value `val`.  Lua value is of the form {Type=f(vt), Value=val}.
  helper function for com2lua.
*/
void tLuaCOMTypeHandler::pushTableVarNumber(lua_State *L, VARTYPE vt, double val) {
  pushTableVariantHeader(L, vt);
  lua_pushnumber(L, val);
  lua_settable(L, -3);
}

void tLuaCOMTypeHandler::pushTableVarInteger(lua_State *L, VARTYPE vt, LONGLONG val) {
  pushTableVariantHeader(L, vt);
  pushSignedInteger(L, val);
  lua_settable(L, -3);
}

void tLuaCOMTypeHandler::pushTableVarUnsignedInteger(lua_State *L, VARTYPE vt, ULONGLONG val) {
  pushTableVariantHeader(L, vt);
  pushUnsignedInteger(L, val);
  lua_settable(L, -3);
}


/**
  Pushes onto Lua stack representation of VARIANTARG.
  If is_variant, then Lua value will be in table form (e.g. `{Type=...}`),
  which is more explicit.
*/
// Own conversion temporaries outside the Lua protected frame: allocation
// failures use longjmp and would otherwise bypass their C++ destructors.
struct tLuaCOMTypeHandler::ConversionContext
{
  ConversionContext(tLuaCOMTypeHandler* handler, VARIANTARG original, bool explicit_type)
    : handler(handler), original(original), explicit_type(explicit_type), locked_array(NULL) {}

  ~ConversionContext()
  {
    // A locked SAFEARRAY cannot be destroyed by the VariantValue members.
    if(locked_array)
      SafeArrayUnaccessData(locked_array);
  }

  tLuaCOMTypeHandler* handler;
  VARIANTARG original; // borrowed; the caller owns this value
  bool explicit_type;
  VariantValue value, converted, formatted_date;
  tStringBuffer text;
  std::vector<wchar_t> formatted;
  tLuaVector array_vector;
  std::vector<SAFEARRAYBOUND> array_bounds;
  std::vector<long> array_dimensions, array_indices;
  VariantValue array_element;
  tBstrArray record_field_names;
  VariantValue record_field;
  SAFEARRAY* locked_array;
  std::exception_ptr exception;
};

void tLuaCOMTypeHandler::com2lua(lua_State* L, VARIANTARG varg_orig, bool is_variant)
{
  const int top = lua_gettop(L);
  if(!lua_checkstack(L, 2))
    LUACOM_EXCEPTION(MALLOC_ERROR);

  int status;
  std::exception_ptr exception;
  {
    ConversionContext context(this, varg_orig, is_variant);
    // These Lua allocations happen before the context acquires native data.
    lua_pushlightuserdata(L, &context);
    lua_pushcclosure(L, protectedCom2Lua, 1);
    status = lua_pcall(L, 0, 1, 0);
    exception = context.exception;
  }
  if(exception)
  {
    lua_settop(L, top);
    std::rethrow_exception(exception);
  }
  if(status != 0)
    lua_error(L);
}

int tLuaCOMTypeHandler::protectedCom2Lua(lua_State* L)
{
  ConversionContext* context = static_cast<ConversionContext*>(
    lua_touserdata(L, lua_upvalueindex(1)));
  try
  {
    context->handler->com2luaImpl(L, *context);
    return 1;
  }
  catch(const tLuaCOMException&)
  {
    context->exception = std::current_exception();
    return 0;
  }
  catch(const std::exception&)
  {
    context->exception = std::current_exception();
    return 0;
  }
}

void tLuaCOMTypeHandler::com2luaImpl(lua_State* L, ConversionContext& context)
{
  LUASTACK_SET(L);
  LuaStackGuard stack_guard(L);

  HRESULT hr = S_OK;

  VARIANT& varg = context.value.value;
  const bool is_variant = context.explicit_type;

  lua_getglobal(L, "luacom");
  lua_pushstring(L, "TableVariants");
  getLuaTableValue(L, -2);
  bool table_variants = lua_toboolean(L, -1) != 0;
  lua_pop(L, 2);


  // dereferences VARIANTARG (if necessary)
  hr = VariantCopyInd(&varg, &context.original);
  if (FAILED(hr))
    COM_ERROR(tUtil::GetErrorMessage(hr));

  // Gives a different treatment to SAFEARRAYs
  if (varg.vt & VT_ARRAY)
  {
    // treats an array of VT_UI1 as an array of char and
    // converts it to a string
    if (varg.vt == (VT_ARRAY | VT_UI1))
      safearray2string(L, varg, context);
    else
      safearray_com2lua(L, varg, context);
    VariantClear(&varg);
  }
  else
  {
    // used in some type conversions
    VARIANTARG& new_varg = context.converted.value;

    try
    {
      switch (varg.vt)
      {
      case VT_EMPTY:
        if(is_variant && table_variants)
        {
          lua_newtable(L);
          lua_pushstring(L, "Type");
          lua_pushstring(L, "empty");
          lua_settable(L, -3);
        }
        else lua_pushnil(L);
        break;
      case VT_NULL:      // SQL's NULL value.
        if(is_variant && table_variants)
        {
          lua_newtable(L);
          lua_pushstring(L, "Type");
          lua_pushstring(L, "null");
          lua_settable(L, -3);
        }
        else lua_pushnil(L);
        break;
      case VT_I8:
        if(is_variant && table_variants)
          pushTableVarInteger(L, varg.vt, varg.llVal);
        else
          pushSignedInteger(L, varg.llVal);
        break;
      case VT_UI8:
        if(is_variant && table_variants)
          pushTableVarUnsignedInteger(L, varg.vt, varg.ullVal);
        else
          pushUnsignedInteger(L, varg.ullVal);
        break;
      case VT_I1:
      case VT_I2:
      case VT_I4:
      case VT_INT:
        if(is_variant && table_variants)
          pushTableVarInteger(L, varg.vt, signedVariantValue(varg));
        else
          pushSignedInteger(L, signedVariantValue(varg));
        break;
      case VT_UI1:
      case VT_UI2:
      case VT_UI4:
      case VT_UINT:
        if(is_variant && table_variants)
          pushTableVarUnsignedInteger(L, varg.vt, unsignedVariantValue(varg));
        else
          pushUnsignedInteger(L, unsignedVariantValue(varg));
        break;
      case VT_CY:
      case VT_DECIMAL:
        {
          const int decimalformat = decimalFormat(L);
          if(decimalformat != DECIMAL_FORMAT_NUMBER)
          {
            const bool integers = decimalformat == DECIMAL_FORMAT_EXACT;
            if(is_variant && table_variants)
            {
              pushTableVariantHeader(L, varg.vt);
              pushExactDecimal(L, varg, integers);
              lua_settable(L, -3);
            }
            else
              pushExactDecimal(L, varg, integers);
            break;
          }
        }
        // DecimalFormat "number" converts through a double.
      case VT_R4:
      case VT_R8:
        {
          new_varg.vt = VT_R8;
          HRESULT hr = VariantChangeType(&new_varg, &varg, 0, VT_R8);
          CHK_COM_CODE(hr);

          if(is_variant && table_variants)
            pushTableVarNumber(L, varg.vt, new_varg.dblVal);
          else
            lua_pushnumber(L, new_varg.dblVal);
          break;
        }

      case VT_DATE:
        {
          const int dateformat = dateFormat(L);
          if(dateformat == DATE_FORMAT_STRING)
          {
            HRESULT hr = VariantChangeType(&new_varg, &varg, 0, VT_BSTR);
            CHK_COM_CODE(hr);

            context.text = tUtil::bstr2string(new_varg.bstrVal);
            lua_pushstring(L, context.text);
          }
          else if(dateformat == DATE_FORMAT_TABLE)
          {
            SYSTEMTIME date;
            if(!tUtil::VariantTimeToSystemTimeWithMilliseconds(varg.date, &date))
              COM_ERROR("Cannot convert COM date to system time.");
            lua_newtable(L);
            lua_pushstring(L, "Day");
            lua_pushinteger(L, date.wDay);
            lua_settable(L, -3);
            lua_pushstring(L, "DayOfWeek");
            lua_pushinteger(L, date.wDayOfWeek);
            lua_settable(L, -3);
            lua_pushstring(L, "Month");
            lua_pushinteger(L, date.wMonth);
            lua_settable(L, -3);
            lua_pushstring(L, "Year");
            lua_pushinteger(L, date.wYear);
            lua_settable(L, -3);
            lua_pushstring(L, "Hour");
            lua_pushinteger(L, date.wHour);
            lua_settable(L, -3);
            lua_pushstring(L, "Minute");
            lua_pushinteger(L, date.wMinute);
            lua_settable(L, -3);
            lua_pushstring(L, "Second");
            lua_pushinteger(L, date.wSecond);
            lua_settable(L, -3);
            lua_pushstring(L, "Milliseconds");
            lua_pushinteger(L, date.wMilliseconds);
            lua_settable(L, -3);
          }
          else if(dateformat == DATE_FORMAT_ISO)
          {
            SYSTEMTIME date;
            if(!tUtil::VariantTimeToSystemTimeWithMilliseconds(varg.date, &date))
              COM_ERROR("Cannot convert COM date to system time.");
            char text[32];
            const int length = snprintf(
              text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
              static_cast<unsigned>(date.wYear),
              static_cast<unsigned>(date.wMonth),
              static_cast<unsigned>(date.wDay),
              static_cast<unsigned>(date.wHour),
              static_cast<unsigned>(date.wMinute),
              static_cast<unsigned>(date.wSecond),
              static_cast<unsigned>(date.wMilliseconds));
            if(length <= 0 || length >= static_cast<int>(sizeof(text)))
              COM_ERROR("Cannot format COM date.");
            lua_pushlstring(L, text, length);
          }
          else // DATE_FORMAT_STRING_MS_ACCURATE
          {
            SYSTEMTIME date;
            if(!tUtil::VariantTimeToSystemTimeWithMilliseconds(varg.date, &date))
              COM_ERROR("Cannot convert COM date to system time.");
            if(!tUtil::RoundSystemTimeToNearestSecond(&date))
              COM_ERROR(tUtil::GetErrorMessage(GetLastError()));

            if(date.wYear < 1601)
            {
              double rounded_date;
              if(!tUtil::SystemTimeToVariantTimeWithMilliseconds(
                   date, &rounded_date))
                COM_ERROR("Cannot convert system time to COM date.");

              VARIANTARG rounded_varg;
              VariantInit(&rounded_varg);
              rounded_varg.vt = VT_DATE;
              rounded_varg.date = rounded_date;
              HRESULT hr = VariantChangeType(
                &new_varg, &rounded_varg, 0, VT_BSTR);
              CHK_COM_CODE(hr);
              context.text = tUtil::bstr2string(new_varg.bstrVal);
              lua_pushstring(L, context.text);
              break;
            }

            const int date_length = GetDateFormatW(
              LOCALE_USER_DEFAULT, DATE_SHORTDATE, &date, NULL, NULL, 0);
            const int time_length = GetTimeFormatW(
              LOCALE_USER_DEFAULT, 0, &date, NULL, NULL, 0);
            if(date_length == 0 || time_length == 0)
              COM_ERROR(tUtil::GetErrorMessage(GetLastError()));

            std::vector<wchar_t>& formatted = context.formatted;
            formatted.resize(date_length + time_length);
            if(GetDateFormatW(
                 LOCALE_USER_DEFAULT, DATE_SHORTDATE, &date, NULL,
                 &formatted[0], date_length) == 0)
              COM_ERROR(tUtil::GetErrorMessage(GetLastError()));

            formatted[date_length - 1] = L' ';
            if(GetTimeFormatW(
                 LOCALE_USER_DEFAULT, 0, &date, NULL,
                 &formatted[date_length], time_length) == 0)
              COM_ERROR(tUtil::GetErrorMessage(GetLastError()));

            VariantValue& formatted_date = context.formatted_date;
            formatted_date.value.vt = VT_BSTR;
            formatted_date.value.bstrVal = SysAllocString(&formatted[0]);
            CHKMALLOC(formatted_date.value.bstrVal);
            context.text = tUtil::bstr2string(formatted_date.value.bstrVal);
            lua_pushstring(L, context.text);
          }

          break;
        }


      case VT_ERROR:
        // assumes that a parameter has been omitted
        if(is_variant && table_variants) {
            lua_newtable(L);
            lua_pushstring(L, "Type");
            lua_pushstring(L, "error");
            lua_settable(L, -3);
        }
  else
          lua_pushnil(L);
        break;

      case VT_BOOL:
        if(is_variant && table_variants) {
            lua_newtable(L);
            lua_pushstring(L, "Type");
            lua_pushstring(L, "bool");
            lua_settable(L, -3);
            lua_pushstring(L, "Value");
            lua_pushboolean(L, varg.boolVal);
            lua_settable(L, -3);
        }
  else
          lua_pushboolean(L, varg.boolVal);
        break;

      case VT_BSTR:
        {
          context.text = tUtil::bstr2string(varg.bstrVal);
          tStringBuffer& str = context.text;
            if(is_variant && table_variants) {
              lua_newtable(L);
            lua_pushstring(L, "Type");
            lua_pushstring(L, "string");
            lua_settable(L, -3);
            lua_pushstring(L, "Value");
            lua_pushlstring(L, str, str.getSize()-1);
            lua_settable(L, -3);
          } else {
              lua_pushlstring(L, str, str.getSize()-1);
          }

          break;
        }

      case VT_DISPATCH:
        {
          unsigned int ninfo = 0;
          IDispatch *pdisp = varg.pdispVal;

          if(pdisp == NULL)
          {
            lua_pushnil(L);
            break;
          }

          if(pushLuaDispatchIfSameState(L, pdisp))
            break;

          tLuaCOM* lcom = NULL;

          try
          {
            lcom = tLuaCOM::CreateLuaCOM(L, pdisp);
          }
          catch(class tLuaCOMException& e)
          {
            UNUSED(e);
            lua_pushnil(L);
            break;
          }

          LuaBeans::push(L, lcom);
        }
        break;

      case VT_UNKNOWN:
        {
          // first, tries to get an IDispatch. If not possible,
          // pushes pointer
          IUnknown* punk = varg.punkVal;
  
          tCOMPtr<IDispatch> pdisp;
          hr = punk->QueryInterface(IID_IDispatch, (void **) &pdisp);

          if(SUCCEEDED(hr))
          {
            if(pushLuaDispatchIfSameState(L, pdisp))
              break;

               tLuaCOM* lcom = NULL;

            try { lcom = tLuaCOM::CreateLuaCOM(L, pdisp); }
            catch(class tLuaCOMException& e)
            {
              UNUSED(e);
              lua_pushnil(L);
              break;
            }

            LuaBeans::push(L, lcom);
            break;
          }

          tCOMPtr<IEnumVARIANT> pEV;
          hr = punk->QueryInterface(IID_IEnumVARIANT, (void **) &pEV);
          if(SUCCEEDED(hr))
          {
            tLuaCOMEnumerator* enumerator = new tLuaCOMEnumerator(pEV);

            enumerator->push(L);
            break;
          }

          // defaults to pushing and userdata for the IUnknown
          pushIUnknown(L, varg.punkVal);
          break;
        }

      case VT_RECORD:
        {
          if(varg.pRecInfo == NULL || varg.pvRecord == NULL)
            TYPECONV_ERROR("COM record has no record information or data.");

          ULONG field_count = 0;
          hr = varg.pRecInfo->GetFieldNames(&field_count, NULL);
          CHK_COM_CODE(hr);

          tBstrArray& field_names = context.record_field_names;
          field_names.resize(field_count);
          ULONG returned_count = field_count;
          hr = varg.pRecInfo->GetFieldNames(&returned_count, field_names.data());
          CHK_COM_CODE(hr);

          lua_newtable(L);
          for(ULONG i = 0; i < returned_count; i++)
          {
            VARIANT& field = context.record_field.value;
            hr = varg.pRecInfo->GetField(varg.pvRecord, field_names[i], &field);
            CHK_COM_CODE(hr);

            // Lua allocation errors bypass local C++ destructors. Keep both
            // the copied field and its converted name in the outer context.
            context.text = tUtil::bstr2string(field_names[i]);
            lua_pushstring(L, context.text);
            com2lua(L, field, true);
            lua_settable(L, -3);
            VariantClear(&field);
          }
          break;
        }

      default:
        {
          char msg[100];
          sprintf(msg, "COM->Lua - Type 0x%.2x not implemented.", varg.vt);
          TYPECONV_ERROR(msg);
          break;
        }
      }
    }
    catch(class tLuaCOMException&)
    {
      VariantClear(&varg);
      throw;
    }

    VariantClear(&new_varg);
    VariantClear(&varg);
  }

  LUASTACK_CLEAN(L, 1);
  stack_guard.dismiss();
}


// helper function for lua2com.
tLuaCOM *tLuaCOMTypeHandler::convert_table(lua_State *L, stkIndex luaval)
{
  tLuaCOM *lcom;
  stkIndex table = lua_gettop(L);
  lua_pushvalue(L, luaval);
  lua_pushstring(L, "typelib");
  getLuaTableValue(L, table);
  if(!lua_isnil(L, -1))
  {
    lua_getglobal(L, "luacom");
    lua_pushstring(L, "ImplInterfaceFromTypelib");
    getLuaTableValue(L, -2);
    lua_remove(L, -2);
    lua_insert(L, table+1);
    lua_pushstring(L, "interface");
    getLuaTableValue(L, table);
    lua_pushstring(L, "coclass");
    getLuaTableValue(L, table);
    if(lua_isnil(L, -1)) {
      lua_pop(L, 1);
      callLuaFunction(L, 3, 1);
    } else {
      callLuaFunction(L, 4, 1);
    }
  }
  else
  {
    lua_pop(L, 1);
    lua_getglobal(L, "luacom");
    lua_pushstring(L, "ImplInterface");
    getLuaTableValue(L, -2);
    lua_remove(L, -2);
    lua_insert(L, table+1);
    lua_pushstring(L, "progid");
    getLuaTableValue(L, table);
    lua_pushstring(L, "interface");
    getLuaTableValue(L, table);
    callLuaFunction(L, 3, 1);
  }
  lcom = from_lua(L, lua_gettop(L));
  lua_settop(L, table-1);
  return lcom;
}


/**
  Sets VARIANTARG to stack-based Lua value (luaval) converted to VARTYPE.

  note: type only used when luaCompat_checkTagToCom(L, luaval)
*/
void tLuaCOMTypeHandler::lua2com(lua_State* L, stkIndex luaval, VARIANTARG& varg, VARTYPE type)
{
  LUASTACK_SET(L);
  LuaStackGuard stack_guard(L);

  CHECKPARAM(luaval > 0);

  VariantClear(&varg);

  switch(lua_type(L, luaval))
  {
  case LUA_TNUMBER:
    if(type == VT_I8)
    {
      if(!luaValueToSignedInteger(L, luaval, &varg.llVal))
        TYPECONV_ERROR("int8 numbers must be exact integers in the Lua numeric range.");
      varg.vt = VT_I8;
      break;
    }
    if(type == VT_UI8)
    {
      if(!luaValueToUnsignedInteger(L, luaval, &varg.ullVal))
        TYPECONV_ERROR("uint8 numbers must be exact non-negative integers in the Lua numeric range.");
      varg.vt = VT_UI8;
      break;
    }
#if LUA_VERSION_NUM >= 503
    // Integers: VT_I4 where they fit, VT_R8 while a double is exact,
    // VT_I8 beyond.
    if(lua_isinteger(L, luaval))
    {
      const lua_Integer value = lua_tointeger(L, luaval);
      if(value >= -2147483647 - 1 && value <= 2147483647)
      {
        varg.lVal = static_cast<LONG>(value);
        varg.vt = VT_I4;
        break;
      }
      if(value < -9007199254740992LL || value > 9007199254740992LL)
      {
        varg.llVal = static_cast<LONGLONG>(value);
        varg.vt = VT_I8;
        break;
      }
    }
#endif
    varg.dblVal = lua_tonumber(L, luaval);
    varg.vt = VT_R8;
    break;


  case LUA_TSTRING:
    {
      size_t l_len = 0;
      const char* value = lua_tolstring(L, luaval, &l_len);
      if(type == VT_I8)
      {
        if(!parseSignedDecimal(value, l_len, &varg.llVal))
          TYPECONV_ERROR("int8 strings must contain one decimal integer in range.");
        varg.vt = VT_I8;
      }
      else if(type == VT_UI8)
      {
        if(!parseUnsignedDecimal(value, l_len, ~static_cast<ULONGLONG>(0),
                                 &varg.ullVal))
          TYPECONV_ERROR("uint8 strings must contain one non-negative decimal integer in range.");
        varg.vt = VT_UI8;
      }
      else
      {
        tStringBuffer str;
        str.copyToBuffer(value, l_len);
        varg.bstrVal = tUtil::string2bstr(str, l_len);
        varg.vt = VT_BSTR;
      }
    }
    break;


  case LUA_TTABLE:
    {
    // tests whether it's a LuaCOM object
    tLuaCOM* lcom = from_lua(L, luaval);

    if(lcom)
    {
      varg.pdispVal = lcom->GetIDispatch();

      varg.pdispVal->AddRef();
      varg.vt = VT_DISPATCH;
    }
    else
    {
      // tests if it is an already created tLuaDispatch
      lua_pushlightuserdata(L, idxDispatch);
      lua_rawget(L, luaval);
      if(!lua_isnil(L, -1)) {
        varg.pdispVal = (IDispatch*)luaCompat_getPointer(L, -1);
        varg.pdispVal->AddRef();
        varg.vt = VT_DISPATCH;
      }
      else if(luaCompat_checkTagToCom(L, luaval))
      {
        lua_remove(L, -2);
        tLuaCOM* lcom;
        switch(lua_type(L, -1)) {
          case LUA_TTABLE:
            lcom = convert_table(L, luaval);
            break;
          case LUA_TFUNCTION:
            lua_pushvalue(L, luaval);
            lua_pushnumber(L, type);
            callLuaFunction(L, 2, 1);
            lcom = from_lua(L, lua_gettop(L));
            lua_pop(L, 1);
            break;
          default:
            lua_pop(L,1);
            TYPECONV_ERROR("Invalid conversion function.");
        }
        if(!lcom) TYPECONV_ERROR("Conversion function failed.");
        varg.pdispVal = lcom->GetIDispatch();
        varg.pdispVal->AddRef();
        varg.vt = VT_DISPATCH;
      }
      else {
        lua_pushstring(L, "Type");
        getLuaTableValue(L, luaval);
        if(!lua_isnil(L, -1)) { // Table describes a variant
          tStringBuffer vtype(lua_tostring(L, -1));
          lua_pushstring(L, "Value");
          getLuaTableValue(L, luaval);
          if(strcmp(vtype, "double") == 0) {
            varg.vt = VT_R8;
            varg.dblVal = lua_tonumber(L, -1);
          } else if(strcmp(vtype, "array") == 0) {
            safearray_lua2com(L, lua_gettop(L), varg, VT_VARIANT);
          } else if(strncmp(vtype, "array of ", strlen("array of ")) == 0) {
            VARTYPE vtarr = VT_EMPTY;
            const char *varrtype = vtype + strlen("array of ");
            for (int i=0; com_type_names[i]; i++) {
              if (strcmp(com_type_names[i], varrtype) == 0) {
                vtarr = com_types[i];
                break;
              }
            }
            safearray_lua2com(L, lua_gettop(L), varg, vtarr);
          } else if(strcmp(vtype, "float") == 0) {
            varg.vt = VT_R4;
            varg.fltVal = (float)lua_tonumber(L, -1);
          } else if(strcmp(vtype, "currency") == 0) {
            luaValueToCurrency(L, -1, varg);
          } else if(strcmp(vtype, "decimal") == 0) {
            luaValueToDecimal(L, -1, varg);
          } else if(strcmp(vtype, "int8") == 0) {
            varg.vt = VT_I8;
            if(lua_type(L, -1) == LUA_TSTRING) {
              size_t length = 0;
              const char* value = lua_tolstring(L, -1, &length);
              if(!parseSignedDecimal(value, length, &varg.llVal))
                TYPECONV_ERROR("int8 strings must contain one decimal integer in range.");
            }
            else if(!luaValueToSignedInteger(L, -1, &varg.llVal))
              TYPECONV_ERROR("int8 values must be exact integers or decimal strings in range.");
          } else if(strcmp(vtype, "uint8") == 0) {
            varg.vt = VT_UI8;
            if(lua_type(L, -1) == LUA_TSTRING) {
              size_t length = 0;
              const char* value = lua_tolstring(L, -1, &length);
              if(!parseUnsignedDecimal(value, length,
                                       ~static_cast<ULONGLONG>(0),
                                       &varg.ullVal))
                TYPECONV_ERROR("uint8 strings must contain one non-negative decimal integer in range.");
            }
            else if(!luaValueToUnsignedInteger(L, -1, &varg.ullVal))
              TYPECONV_ERROR("uint8 values must be exact non-negative integers or decimal strings in range.");
          } else if(strcmp(vtype, "int4") == 0) {
            LONGLONG value = 0;
            if(!luaValueToSignedIntegerOrString(L, -1, &value) ||
               value < -2147483647LL - 1 || value > 2147483647LL)
              TYPECONV_ERROR("int4 values must be exact integers in range.");
            varg.vt = VT_I4;
            varg.lVal = static_cast<LONG>(value);
          } else if(strcmp(vtype, "uint4") == 0) {
            ULONGLONG value = 0;
            if(!luaValueToUnsignedIntegerOrString(L, -1, &value) ||
               value > 4294967295ULL)
              TYPECONV_ERROR("uint4 values must be exact non-negative integers in range.");
            varg.vt = VT_UI4;
            varg.ulVal = static_cast<ULONG>(value);
          } else if(strcmp(vtype, "int2") == 0) {
            LONGLONG value = 0;
            if(!luaValueToSignedIntegerOrString(L, -1, &value) ||
               value < SHRT_MIN || value > SHRT_MAX)
              TYPECONV_ERROR("int2 values must be exact integers in range.");
            varg.vt = VT_I2;
            varg.iVal = static_cast<SHORT>(value);
          } else if(strcmp(vtype, "uint2") == 0) {
            ULONGLONG value = 0;
            if(!luaValueToUnsignedIntegerOrString(L, -1, &value) ||
               value > USHRT_MAX)
              TYPECONV_ERROR("uint2 values must be exact non-negative integers in range.");
            varg.vt = VT_UI2;
            varg.uiVal = static_cast<USHORT>(value);
          } else if(strcmp(vtype, "int1") == 0) {
            LONGLONG value = 0;
            if(!luaValueToSignedIntegerOrString(L, -1, &value) ||
               value < SCHAR_MIN || value > SCHAR_MAX)
              TYPECONV_ERROR("int1 values must be exact integers in range.");
            varg.vt = VT_I1;
            varg.cVal = static_cast<CHAR>(value);
          } else if(strcmp(vtype, "uint1") == 0) {
            ULONGLONG value = 0;
            if(!luaValueToUnsignedIntegerOrString(L, -1, &value) ||
               value > UCHAR_MAX)
              TYPECONV_ERROR("uint1 values must be exact non-negative integers in range.");
            varg.vt = VT_UI1;
            varg.bVal = static_cast<BYTE>(value);
          } else if(strcmp(vtype, "int") == 0) {
            LONGLONG value = 0;
            if(!luaValueToSignedIntegerOrString(L, -1, &value) ||
               value < INT_MIN || value > INT_MAX)
              TYPECONV_ERROR("int values must be exact integers in range.");
            varg.vt = VT_INT;
            varg.intVal = static_cast<INT>(value);
          } else if(strcmp(vtype, "uint") == 0) {
            ULONGLONG value = 0;
            if(!luaValueToUnsignedIntegerOrString(L, -1, &value) ||
               value > UINT_MAX)
              TYPECONV_ERROR("uint values must be exact non-negative integers in range.");
            varg.vt = VT_UINT;
            varg.uintVal = static_cast<UINT>(value);
          } else if(strcmp(vtype, "string") == 0) {
            size_t length = 0;
            const char* value = lua_tolstring(L, -1, &length);
            varg.bstrVal = tUtil::string2bstr(value, length);
            varg.vt = VT_BSTR;
          } else if(strcmp(vtype, "null") == 0) {
            varg.vt = VT_NULL;
          } else if(strcmp(vtype, "error") == 0) {
            varg.vt = VT_ERROR;
            varg.scode = (SCODE)lua_tonumber(L, -1);
          } else if(strcmp(vtype, "bool") == 0) {
            varg.vt = VT_BOOL;
            varg.boolVal = lua_toboolean(L, -1) ? VARIANT_TRUE : VARIANT_FALSE;
          } else {
            varg.vt = VT_EMPTY;
          }
        }
        else { // maybe a date?
          int isdate = 0;
          SYSTEMTIME date;
          lua_pushstring(L, "Day");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wDay = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          lua_pushstring(L, "DayOfWeek");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wDayOfWeek = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          lua_pushstring(L, "Month");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wMonth = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          lua_pushstring(L, "Year");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wYear = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          lua_pushstring(L, "Hour");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wHour = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          lua_pushstring(L, "Minute");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wMinute = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          lua_pushstring(L, "Second");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wSecond = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          lua_pushstring(L, "Milliseconds");
          getLuaTableValue(L, luaval);
          isdate = isdate || !lua_isnil(L, -1);
          date.wMilliseconds = (WORD)lua_tonumber(L, -1);
          lua_pop(L, 1);
          if(isdate) {
            varg.vt = VT_DATE;
            if(!tUtil::SystemTimeToVariantTimeWithMilliseconds(date, &varg.date))
              COM_ERROR("Cannot convert system time to COM date.");
          } else safearray_lua2com(L, luaval, varg, VT_VARIANT);
        }
      }
    }
      break;
    }

    break;

  case LUA_TBOOLEAN:
    varg.vt = VT_BOOL;
    varg.boolVal = lua_toboolean(L, luaval) ? VARIANT_TRUE : VARIANT_FALSE;
    break;


  case LUA_TUSERDATA:
    if(isIUnknown(L, luaval)) // is an IUnknown?
    {
      varg.vt = VT_UNKNOWN;
      varg.punkVal = (IUnknown *)*(void **)lua_touserdata(L, luaval);
      varg.punkVal->AddRef();
    }
    break;

  case LUA_TNIL:
    varg.vt = VT_EMPTY;
    break;

  case LUA_TNONE:
  default:
    TYPECONV_ERROR("No Lua value to convert.");
    break;
  }
  LUASTACK_DOCLEAN(L, 0);
}

/**
 Puts value from Lua stack into VARIANTARG.
 NOTE: uses VARTYPE funcdesc->elemdescFunc.tdesc.vt
 (IMPROVE: don't need to pass entire FUNCDESC.  Not using other parameters of TYPEDESC?)
*/
bool tLuaCOMTypeHandler::setRetval(lua_State* L,
                                   const FUNCDESC * funcdesc,
                                   stkIndex luaval,
                                   VARIANTARG * pvarg_retval)
{
  VariantValue varg;

  if(funcdesc->elemdescFunc.tdesc.vt != VT_VOID)
  {
    lua2com(L, luaval, varg.value, funcdesc->elemdescFunc.tdesc.vt);

    initByRefParam(pvarg_retval, VT_VARIANT);
    toByRefParam(varg.value, pvarg_retval);
    varg.detach();
  }
  else
    return false;

  return true;
}


/**
 Pushes onto the Lua stack any "out" parameters stored in the `dispparams` list.
 Returns the number of values pushed.
 The optional `pfuncdesc` (may be nullptr) helps interpret whether parameters
 are "out" parameters.
*/
int tLuaCOMTypeHandler::pushOutValues(lua_State* L, const DISPPARAMS& dispparams, const FUNCDESC* pfuncdesc)
{
  CHECKPRECOND(!pfuncdesc || dispparams.cArgs == pfuncdesc->cParams);

  // Iterate over parameters.
  int num_pushed_values = 0;
  for(unsigned i = 0; i < dispparams.cArgs; i++)
  {
    VARIANTARG& varg = dispparams.rgvarg[dispparams.cArgs - i - 1];
    bool isout = (pfuncdesc &&
      ((pfuncdesc->lprgelemdescParam[i].paramdesc.wParamFlags & PARAMFLAG_FOUT) != 0)) ||
       ((varg.vt & VT_BYREF) != 0);
    if (isout)
    {
      com2lua(L, varg);
      num_pushed_values++;
    }
  }

  return num_pushed_values;
}


/**
  Destroys DISPPARAMS list.
*/
void tLuaCOMTypeHandler::releaseVariants(DISPPARAMS *pDispParams)
{
  VARIANTARG* &vargs = pDispParams->rgvarg;
  if (vargs != NULL)
  {
    // Release memory.
    for (unsigned int i = 0; i < pDispParams->cArgs; i++)
    {
      releaseVariant(&vargs[i]);
    }
    delete [] vargs;
    
    // Optional zeroing of memory (not necessary).
    vargs = NULL;
    pDispParams->cArgs = 0;
    // pDispParams->rgdispidNamedArgs = NULL; (this memory is not currently owned by object).
    // pDispParams->cNamedArgs = 0;
  }
}



/**
 Fills DISPPARAMS structure from values on Lua stack (`params`).
 Optional `pfuncdesc` helps define proper conversions.
 invkind if the type of function call the parameters are for.
*/
void tLuaCOMTypeHandler::fillDispParams(lua_State* L,
                                        DISPPARAMS& rDispParams,
                                        FUNCDESC * pfuncdesc,
                                        tLuaObjList params,
                                        int invkind)
{
  // initializes DISPPARAMS
  rDispParams.cArgs = 0;
  rDispParams.cNamedArgs = 0;
  rDispParams.rgvarg = NULL;
  rDispParams.rgdispidNamedArgs = NULL;

  // if we know that this function does not received
  // parameters, stop here
  if (pfuncdesc && pfuncdesc->cParams == 0)
    return;

  static DISPID dispidNamed = DISPID_PROPERTYPUT;  // effectively const

  // references to simplify code
  unsigned int& r_cArgs = rDispParams.cArgs;
  VARIANTARG* &r_rgvarg  = rDispParams.rgvarg;

  // propertyput particular case
  if(invkind == DISPATCH_PROPERTYPUT ||
     invkind == DISPATCH_PROPERTYPUTREF)
  {
    rDispParams.cNamedArgs = 1;
    rDispParams.rgdispidNamedArgs = &dispidNamed;
  }

  r_cArgs = 0; // starts empty
  long lua_args = 0;
  long max_idl_params = 0;


  // first attemp to determine the size of DISPPARAMS
  unsigned int max_params = 0;

  if(pfuncdesc)
    max_params = pfuncdesc->cParams;
  else
    max_params = params.getNumParams();

  // if this function can receive a variable
  // number of arguments, then the last
  // formal parameter is a safearray of these
  // arguments and must be treated separately.
  // Following the documentation, we should pass
  // a safearray containing the remaining parameters.
  // As this didn't work with MSHTML.HTMLDocument.WriteLn,
  // we choose to do the same as VBScript: pass all
  // the remaining parameters separately.
  // The code to do this the "documented" way
  // is commented some lines of code below.
  if(pfuncdesc && pfuncdesc->cParamsOpt == -1)
  {
    assert(max_params >= 1);

    // The maximum number of parameters now is
    // bounded by the size of the parameter list,
    // if it is larger than the formal parameter
    // list
    if(params.getNumParams() > pfuncdesc->cParams)
      max_params = params.getNumParams();
    else
      max_params = pfuncdesc->cParams;

    // We ignore the last parameter (the safearray
    // of variants), as we won't use it
    max_idl_params = pfuncdesc->cParams - 1;
  }
  else
    max_idl_params = max_params;

  // creates array of VARIANTs
  r_rgvarg = new VARIANTARG[max_params];
  unsigned int initialized_args = 0;

  // Iterate the array lprgelemdescParam, getting parameters
  // from the Lua table.

  try
  {
    for (unsigned int i = 0; i < max_params; i++)
    {
      VariantValue defaultValue;
      VariantValue var;

      // default values
      bool byref      = true;
      bool hasdefault = false;
      bool array      = false;
      bool optin      = false;
      VARTYPE type = VT_VARIANT;
      VARTYPE array_type = VT_EMPTY;

      VariantInit(&r_rgvarg[r_cArgs]);
      initialized_args = r_cArgs + 1;

      // processing that makes sense when there is type info available
      if(pfuncdesc && i < max_idl_params)
      {
        PARAMDESC paramdesc = pfuncdesc->lprgelemdescParam[i].paramdesc;
        const TYPEDESC tdesc =
          processTYPEDESC(m_typeinfo, pfuncdesc->lprgelemdescParam[i].tdesc);

        // stores type of the expected value
        type = tdesc.vt & ~VT_BYREF;

        // tests whether an array is expected,
        // storing type of array elements if so.
        if(tdesc.vt & VT_ARRAY)
        {
          array = true;
          array_type = type & ~VT_ARRAY;
        }

        // ignores out parameters
        if(!(paramdesc.wParamFlags &  PARAMFLAG_FIN) &&
            (paramdesc.wParamFlags != PARAMFLAG_NONE) &&
            !(paramdesc.wParamFlags & PARAMFLAG_FOPT))
        {
          // If server expects a safearray, creates an
          // empty one to avoid problems
          if(array)
          {
            r_rgvarg[r_cArgs].vt = array_type | VT_ARRAY | VT_BYREF;
            r_rgvarg[r_cArgs].pparray =
              (SAFEARRAY**) CoTaskMemAlloc(sizeof(SAFEARRAY*));
            CHKMALLOC(r_rgvarg[r_cArgs].pparray);
            *r_rgvarg[r_cArgs].pparray = NULL;
          } else {
            initByRefParam(&r_rgvarg[r_cArgs], type);
          }

          r_cArgs++;
          continue;
        }
        else if(paramdesc.wParamFlags & PARAMFLAG_FOUT)
        {
          // assumes that it is an in/out parameter
          byref = true;
        }
        else if(tdesc.vt & VT_BYREF) // tdesc overrides paramflags
          byref = true;
        else // in param
          byref = false;

        // detects if optional output parameter
        optin = (paramdesc.wParamFlags & PARAMFLAG_FOPT) != 0 &&
                (paramdesc.wParamFlags & PARAMFLAG_FIN) != 0;

        // deals with default values (if any)
        if(paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT)
        {
          hasdefault = true;
          CHK_COM_CODE(VariantCopy(
            &defaultValue.value,
            &paramdesc.pparamdescex->varDefaultValue));
        }
      }

      // gets value from Lua
      stkIndex val = params.getparam(lua_args);

      if(val != 0 && lua_type(L, val) != LUA_TNONE && !lua_isnil(L, val))
      {
        if(array)
          safearray_lua2com(L, val, var.value, array_type);
        else
          lua2com(L, val, var.value, type);
      }
      else if(hasdefault)
      {
        CHK_COM_CODE(VariantCopy(&var.value, &defaultValue.value));
      }
      else if(optin)
      {
        // assumes that a parameter is expected but has not been found
        var.value.vt = VT_ERROR;
        var.value.scode = DISP_E_PARAMNOTFOUND;
        byref = false;
      }
      // else if(optout) do nothing (handled below instead)

      if(!byref)
      {
        CHK_COM_CODE(VariantCopy(&r_rgvarg[i], &var.value));
      }
      else // byref (note: optout => byref)
      {
        initByRefParam(&r_rgvarg[i], type, true);
        toByRefParam(var.value, &r_rgvarg[i]);
        var.detach();
      }

      r_cArgs++;
      lua_args++;
    }

    #if 0
    // deals with vararg functions following
    // vararg documentation
    if(pfuncdesc && pfuncdesc->cParamsOpt == -1)
    {
      safearray_lua2com(
        params.getparam(lua_args),
        r_rgvarg[r_cArgs],
        VT_VARIANT,
        true);

      r_cArgs++;
    }
    #endif
  }
  catch(...)
  {
    for(unsigned int i = 0; i < initialized_args; i++)
      releaseVariant(&r_rgvarg[i]);
    delete[] r_rgvarg;
    r_rgvarg = NULL;
    r_cArgs = 0;
    throw;
  }

  // inverts parameters order to match
  // what is expected by Automation
  if(r_cArgs > 0)
  {
    VARIANTARG temp;

    for(unsigned int i = 0; i < r_cArgs/2; i++)
    {
      temp = r_rgvarg[i];
      r_rgvarg[i] = r_rgvarg[r_cArgs - i - 1];
      r_rgvarg[r_cArgs - i - 1] = temp;
    }
  }

  return;
}


/**
  Pushes onto Lua stack representations of non-"out" parameters of DISPPARAMS.
  This can be used to marshall arguments of a COM method into a Lua function.
*/
void tLuaCOMTypeHandler::pushLuaArgs(lua_State* L,
                                     DISPPARAMS* pDispParams,
                                     const ELEMDESC* pElemDesc)
{
  HRESULT hr = S_OK;

  const unsigned int first_named_param =
    pDispParams->cArgs - pDispParams->cNamedArgs;

  for(unsigned int arg = 0; arg < pDispParams->cArgs; arg++)
  {
    const USHORT& wParamFlags = pElemDesc[arg].paramdesc.wParamFlags;
    const TYPEDESC tdesc = processTYPEDESC(m_typeinfo, pElemDesc[arg].tdesc);

    // if parameter is not an out param, convert it to Lua
    if(wParamFlags & PARAMFLAG_FIN || wParamFlags == PARAMFLAG_NONE )
    {

      // First we have to find the right index in the
      // rgvarg array, as we must deal with positional and
      // named parameters

      int index = -1;
      if(arg < first_named_param) // still inside positional parameters
        index = pDispParams->cArgs - arg - 1;
      else // now we are dealing with named parameters
      {
        // tries to find a named param for the current position
        for(unsigned int i = 0; i < pDispParams->cNamedArgs; i++)
        {
          if(pDispParams->rgdispidNamedArgs[i] == arg)
          {
            index = i;
            break;
          }
        }

        if(index == -1) // no corresponding named param found
        {
          lua_pushnil(L);
          continue;
        }
      }

      VARIANTARG& varg = pDispParams->rgvarg[index];

      // we assume that empty parameters with paramflags NONE
      // are out params
      if(varg.vt == VT_EMPTY && wParamFlags == PARAMFLAG_NONE)
        continue;


      if(varg.vt == VT_ERROR || varg.vt == VT_EMPTY)
      {
        // parameter has been omitted
        lua_pushnil(L);
        continue;
      }

      VariantValue var;

      // removes indirections
      hr = VariantCopyInd(&var.value, &varg);
      CHK_COM_CODE(hr);

      // some type checks
      if((tdesc.vt & ~(VT_BYREF)) != VT_VARIANT &&
            V_ISARRAY(&tdesc) != V_ISARRAY(&var.value))
        CHK_COM_CODE(DISP_E_TYPEMISMATCH);
      else if(!V_ISARRAY(&var.value))
      {
        // coerces value to the expected scalar type
        Coerce(var.value, var.value, (tdesc.vt & ~VT_BYREF));
      }

      com2lua(L, var.value, (tdesc.vt & ~VT_BYREF) == VT_VARIANT);
    }
  }
}


/**
  Copies representations of Lua values starting at stack index `outvalue`
  into only "out" parameters of DISPARAMS.
  FUNCDESC must be provided to assist in interpreting parameters.
*/
void tLuaCOMTypeHandler::setOutValues(lua_State* L,
                                      FUNCDESC * pFuncDesc,
                                      DISPPARAMS * pDispParams,
                                      stkIndex outvalue
                                      )
{
  CHECKPRECOND(outvalue > 0);

  const unsigned int num_args = pDispParams->cArgs;
  const unsigned int first_named_param = pDispParams->cArgs - pDispParams->cNamedArgs;
  unsigned int arg = 0;

  // Search return values of out parameters
  for(arg = 0; arg < num_args; arg++)
  {
    // checks whether there are more return values to map
    // to out parameters
    if(lua_type(L, outvalue) == LUA_TNONE)
      break;

    // alias
    const TYPEDESC tdesc =
      processTYPEDESC(m_typeinfo, pFuncDesc->lprgelemdescParam[arg].tdesc);

    // tests whether this parameters is an out parameter
    if(V_ISBYREF(&tdesc))
    {
      VariantValue var;

      // tries to find the right position in the rgvarg array,
      // when using named args
      unsigned int index = (unsigned int)-1;

      if(arg < first_named_param) // still inside positional parameters
        index = pDispParams->cArgs - arg - 1;
      else // now we are dealing with named parameters
      {
        // tries to find a named param for the current position
        for(unsigned int i = 0; i < pDispParams->cNamedArgs; i++)
        {
          if(pDispParams->rgdispidNamedArgs[i] == arg)
          {
            index = i;
            break;
          }
        }

        if(index == -1)
        {
          // no corresponding named param found, so we must skip this one
          outvalue++;
          continue;
        }
      }

      // alias
      VARIANTARG& varg_orig = pDispParams->rgvarg[index];

      // allocates if pure out value
      USHORT paramFlags = pFuncDesc->lprgelemdescParam[arg].paramdesc.wParamFlags;
      if(!(paramFlags &  PARAMFLAG_FIN) &&
          (paramFlags != PARAMFLAG_NONE) &&
          !(paramFlags & PARAMFLAG_FOPT))
        initByRefParam(&varg_orig, tdesc.vt & ~VT_BYREF);

      // does indirection in the case of VARIANTs
      if(varg_orig.vt == (VT_VARIANT | VT_BYREF))
      {
        VARIANTARG& varg = *varg_orig.pvarVal;

        if(varg.vt & VT_ARRAY)
          safearray_lua2com(L, outvalue, var.value, varg.vt & ~(VT_BYREF | VT_ARRAY));
        else
          lua2com(L, outvalue, var.value, (varg.vt & ~VT_BYREF));

        if(varg.vt & VT_BYREF)
        {
          toByRefParam(var.value, &varg);
          var.detach();
        }
        else
        {
          CHK_COM_CODE(VariantClear(&varg));
          CHK_COM_CODE(VariantCopy(&varg, &var.value));
        }
      }
      else
      {
        VARIANTARG& varg = varg_orig;

        if(varg.vt == VT_ERROR || varg.vt == VT_EMPTY)
          initByRefParam(&varg, tdesc.vt & ~VT_BYREF);

        if(varg.vt & VT_ARRAY)
          safearray_lua2com(L, outvalue, var.value, varg.vt & ~(VT_BYREF | VT_ARRAY));
        else
          lua2com(L, outvalue, var.value, (varg.vt & ~VT_BYREF));

        toByRefParam(var.value, &varg);
        var.detach();
      }

      outvalue++;
    }
  }
}


//
// Conversion of SAFEARRAYs to/from Lua tables.
//

// auxillary functions for safearray_lua2com




SAFEARRAYBOUND* tLuaCOMTypeHandler::getRightOrderedBounds(
    SAFEARRAYBOUND *bounds,
    unsigned long num_dimensions)
{
  SAFEARRAYBOUND* new_bounds = new SAFEARRAYBOUND[num_dimensions];

  for(unsigned long i = 0; i < num_dimensions; i++) {
    new_bounds[i] = bounds[num_dimensions - i - 1];
  }

  return new_bounds;
}


/**
  Puts copy of VARIANT coerced to type `vt` to location `indices` in SAFEARRAY.
*/
void tLuaCOMTypeHandler::put_in_array(SAFEARRAY* safearray,
                         VARIANT var,
                         long* indices,
                         VARTYPE vt
                         )
{
  HRESULT hr = S_OK;

  // Copy first so coercion cannot release storage owned by the caller.
  VARIANT var_value;
  VariantInit(&var_value);
  hr = VariantCopy(&var_value, &var);
  if(FAILED(hr))
  {
    VariantClear(&var_value);
    CHK_COM_CODE(hr);
  }

  try
  {
  Coerce(var_value, var_value, vt);

  switch(vt)
  {
  case VT_I2:
    hr = SafeArrayPutElement(safearray, indices, &var_value.iVal);
    break;

  case VT_I4:
    hr = SafeArrayPutElement(safearray, indices, &var_value.lVal);
    break;

  case VT_I8:
    hr = SafeArrayPutElement(safearray, indices, &var_value.llVal);
    break;

  case VT_UI8:
    hr = SafeArrayPutElement(safearray, indices, &var_value.ullVal);
    break;

  case VT_R4:
    hr = SafeArrayPutElement(safearray, indices, &var_value.fltVal);
    break;

  case VT_R8:
    hr = SafeArrayPutElement(safearray, indices, &var_value.dblVal);
    break;

  case VT_CY:
    hr = SafeArrayPutElement(safearray, indices, &var_value.cyVal);
    break;

  case VT_DATE:
    hr = SafeArrayPutElement(safearray, indices, &var_value.date);
    break;

  case VT_BSTR:
    // BSTR already a pointer
    hr = SafeArrayPutElement(safearray, indices, var_value.bstrVal);
    break;

  case VT_DISPATCH:
    // IDispatch already is a pointer (no indirection need)
    hr = SafeArrayPutElement(safearray, indices, var_value.pdispVal);
    break;

  case VT_ERROR:
    hr = SafeArrayPutElement(safearray, indices, &var_value.scode);
    break;

  case VT_BOOL:
    hr = SafeArrayPutElement(safearray, indices, &var_value.boolVal);
    break;

  case VT_VARIANT:
    hr = SafeArrayPutElement(safearray, indices, &var_value);
    break;

  case VT_UNKNOWN:
    // see IDispatch
    hr = SafeArrayPutElement(safearray, indices, var_value.punkVal);
    break;

  case VT_DECIMAL:
    hr = SafeArrayPutElement(safearray, indices, &var_value.decVal);
    break;

  case VT_UI1:
    hr = SafeArrayPutElement(safearray, indices, &var_value.bVal);
    break;

  case VT_I1:
    hr = SafeArrayPutElement(safearray, indices, &var_value.cVal);
    break;

  case VT_UI2:
    hr = SafeArrayPutElement(safearray, indices, &var_value.uiVal);
    break;

  case VT_UI4:
    hr = SafeArrayPutElement(safearray, indices, &var_value.ulVal);
    break;

  case VT_INT:
    hr = SafeArrayPutElement(safearray, indices, &var_value.intVal);
    break;

  case VT_UINT:
    hr = SafeArrayPutElement(safearray, indices, &var_value.uintVal);
    break;

  case VT_ARRAY:
    TYPECONV_ERROR("SAFEARRAY of SAFEARRAYS not allowed");
    break;

  default:
    TYPECONV_ERROR("Type not compatible with automation.");
    break;
  }

  CHK_COM_CODE(hr);
  }
  catch (...)
  {
    VariantClear(&var_value);
    throw;
  }
  VariantClear(&var_value);
}

stkIndex tLuaCOMTypeHandler::get_from_array(lua_State* L,
                                            SAFEARRAY* safearray,
                                            long *indices,
                                            const VARTYPE& vt,
                                            VARIANTARG& value
                                            )
{
  // The conversion context owns this element across nested Lua errors.
  VariantClear(&value);
  ZeroMemory(&value, sizeof(value)); // safe cleanup if GetElement fails before writing
  void *pv = NULL;
  if(vt == VT_VARIANT)
  {
    pv = &value;
  }
  else
  {
    value.vt = vt;

    // e' uma union, tanto faz de quem pego o ponteiro
    pv = (void *) &value.dblVal;
  }

  HRESULT hr = SafeArrayGetElement(safearray, indices, pv);

  if(FAILED(hr)) {
    LUACOM_EXCEPTION(INTERNAL_ERROR);
  }

  com2lua(L, value);
  VariantClear(&value);

  return lua_gettop(L);
}



bool tLuaCOMTypeHandler::inc_indices(long *indices,
                        SAFEARRAYBOUND *bounds,
                        unsigned long dimensions
                        )
{
  long j = dimensions - 1;

  indices[j]++;

  while(
    (indices[j] >= (long) bounds[j].cElements + bounds[j].lLbound) &&
    (j >= 0)
    )
  {
    indices[j] = bounds[j].lLbound;
    j--;
    if(j < 0 && indices[0] == (long)bounds[0].lLbound) {
      return false;
    }
    indices[j]++;
  }
  return true;
}


/**
 Converts Lua table to SAFEARRAY.
*/
void tLuaCOMTypeHandler::safearray_lua2com(lua_State* L,
                                           stkIndex luaval,
                                           VARIANTARG& varg,
                                           VARTYPE vt,
                                           bool from_stack)
{
  LUASTACK_SET(L);

  HRESULT hr = S_OK;

  tLuaVector luavector;

  long stack_bottom = lua_gettop(L);

  if(!from_stack)
  {
    // when trying to convert a string to a safearray
    // uses a specific method
    if(lua_type(L, luaval) == LUA_TSTRING)
    {
      string2safearray(
        lua_tostring(L, luaval),
        lua_strlen(L, luaval),
        varg
        );
      return;
    }

    // creates a luavector based on the table passed
    luavector.InitVectorFromTable(L, luaval);
  }
  else // trying to create an array from the stack
  {
    luavector.InitVectorFromStack(L, luaval);
  }

  long stack_top = lua_gettop(L);

  if(luavector.getLength() == 0) {
    // returns an empty vector
    varg.vt = vt | VT_ARRAY;
    varg.parray = SafeArrayCreateVector(vt, 0, 0);
    CHECK(varg.parray, INTERNAL_ERROR);

    LUASTACK_CLEAN(L, 0);
    return;
  }

  // create variables
  unsigned long i = 0;
  const unsigned long dimensions = luavector.get_Dimensions();
  SAFEARRAYBOUND *bounds = new SAFEARRAYBOUND[dimensions];
  SAFEARRAY *safearray = NULL;

  VARIANTARG var_value;
  VariantInit(&var_value);

  // initialize dimensions
  for(i = 0; i < dimensions; i++)
  {
    bounds[dimensions - i - 1].lLbound = 0;
    bounds[dimensions - i - 1].cElements = luavector.get_Nth_Dimension(dimensions - i);
  }


  // create array
  safearray = SafeArrayCreate(vt, dimensions, bounds);

  long *indices = NULL;

  try
  {
    CHECK(safearray, INTERNAL_ERROR);

    // initialize indices
    indices = new long[dimensions];

    for(i = 0; i < dimensions; i++)
      indices[i] = bounds[i].lLbound;

    unsigned long dimension = indices[0];
    // copy elements one-by-one
    do
    {
      // obtain value
      luaval = luavector.getindex(indices, dimensions, bounds);

      // convert
      lua2com(L, luaval, var_value, vt);

      // put in array
      put_in_array(safearray, var_value, indices, vt);

      VariantClear(&var_value);
    }
    while(inc_indices(indices, bounds, dimensions)); // incrementa indices
  }
  catch(...)
  {
    VariantClear(&var_value);
    delete[] bounds;
    delete[] indices;
    SafeArrayDestroy(safearray);

    while(stack_top > stack_bottom)
    {
      lua_remove(L, stack_top);
      stack_top--;
    }

    throw;
  }


  // fill varg
  varg.vt = vt | VT_ARRAY;
  varg.parray = safearray;


  delete[] bounds;
  delete[] indices;

  while(stack_top > stack_bottom)
  {
    lua_remove(L, stack_top);
    stack_top--;
  }

  LUASTACK_CLEAN(L, 0);

  return;
}

/**
 Converts array of bytes to SAFEARRAY (of VT_UI1).
 Array is memory block [str, str+len).
*/
void tLuaCOMTypeHandler::string2safearray(const char* str, size_t len, VARIANTARG& varg)
{
  HRESULT hr = S_OK;

  // create array
  SAFEARRAY *safearray = SafeArrayCreateVector(VT_UI1, 0, (ULONG)len);
  CHECK(safearray, INTERNAL_ERROR);

  void * buffer = NULL;
  hr = SafeArrayAccessData(safearray,&buffer);
  if(FAILED(hr))
  {
    SafeArrayDestroy(safearray);
    LUACOM_EXCEPTION(INTERNAL_ERROR);
  }
  if (buffer != NULL)
     memcpy(buffer,str,len);
  SafeArrayUnaccessData(safearray);

  // fill varg
  varg.vt = VT_UI1 | VT_ARRAY;
  varg.parray = safearray;
}

/**
 Converts SAFEARRAY (of VT_UI1) to array of bytes.
 Assumes SAFEARRAY is one-dimensional.
*/
void tLuaCOMTypeHandler::safearray2string(lua_State* L, VARIANTARG & varg,
                                        ConversionContext& context)
{
  CHECKPRECOND(varg.vt & (VT_ARRAY | VT_UI1));
  CHECKPRECOND(varg.parray->cDims == 1);

  HRESULT hr = S_OK;

  long size = varg.parray->rgsabound[0].cElements;

  void * buffer = NULL;
  hr = SafeArrayAccessData(varg.parray, &buffer);
  CHK_COM_CODE(hr);
  context.locked_array = varg.parray;

  lua_pushlstring(L, (char*) buffer, size);

  CHK_COM_CODE(SafeArrayUnaccessData(varg.parray));
  context.locked_array = NULL;
}

/**
 Returns an array containing the number of elements in each dimension
 of a SAFEARRAY.
 Callee owns memory returned.
*/
long* tLuaCOMTypeHandler::dimensionsFromBounds(SAFEARRAYBOUND* bounds,
                                                long num_bounds
                                                )
{
  long* dimensions = new long[num_bounds];

  for(int i = 0; i < num_bounds; i++)
  {
    dimensions[i] = bounds[i].cElements;
  }

  return dimensions;
}



void tLuaCOMTypeHandler::safearray_com2lua(lua_State* L, VARIANTARG & varg,
                                          ConversionContext& context)
{
  CHECK(varg.vt & VT_ARRAY, PARAMETER_OUT_OF_RANGE);
  SAFEARRAY* safearray = varg.parray;
  if(safearray == NULL)
  {
    lua_newtable(L);
    return;
  }

  const int num_dimensions = SafeArrayGetDim(safearray);
  CHECK(num_dimensions > 0, TYPECONV_ERROR);
  if(safearray->rgsabound[num_dimensions - 1].cElements == 0)
  {
    lua_newtable(L);
    return;
  }

  // All native bookkeeping lives outside the Lua protected frame. Building
  // the output table or converting an element can fail with a Lua longjmp.
  context.array_bounds.resize(num_dimensions);
  context.array_dimensions.resize(num_dimensions);
  context.array_indices.resize(num_dimensions);
  SAFEARRAYBOUND* bounds = &context.array_bounds[0];
  long* indices = &context.array_indices[0];
  for(int i = 0; i < num_dimensions; i++)
  {
    bounds[i] = safearray->rgsabound[num_dimensions - i - 1];
    CHECK(bounds[i].cElements <= LONG_MAX, TYPECONV_ERROR);
    context.array_dimensions[i] = static_cast<long>(bounds[i].cElements);
    indices[i] = bounds[i].lLbound;
  }

  tLuaVector& luavector = context.array_vector;
  luavector.InitVectorFromDimensions(&context.array_dimensions[0], num_dimensions);

  VARTYPE vt = VT_EMPTY;
  HRESULT hr = SafeArrayGetVartype(safearray, &vt);
  if(FAILED(hr))
    LUACOM_EXCEPTION(INTERNAL_ERROR);

  const stkIndex stacktop = lua_gettop(L);
  const long elements = luavector.size();
  if(elements < 0 || elements > INT_MAX / 2 ||
     !lua_checkstack(L, static_cast<int>(elements * 2)))
    luaL_error(L, "Insufficient Lua stack space for safearray");

  do
  {
    stkIndex luaval = get_from_array(L, safearray, indices, vt,
                                     context.array_element.value);
    luavector.setindex(L, luaval, indices, num_dimensions, bounds);
  }
  while(inc_indices(indices, bounds, num_dimensions));

  bool succeeded = luavector.CreateTable(L);
  stkIndex clean_until = lua_gettop(L);
  if(succeeded)
    clean_until--; // keep the result table
  while(clean_until > stacktop)
    lua_remove(L, clean_until--);
}


tLuaCOM * tLuaCOMTypeHandler::from_lua(lua_State* L, int index)
{
  return (tLuaCOM *) LuaBeans::from_lua(L, index);
}

TYPEDESC tLuaCOMTypeHandler::processPTR(ITypeInfo* typeinfo,
                                        const TYPEDESC &tdesc)
{
  CHECKPRECOND(tdesc.vt == VT_PTR);

  TYPEDESC pointed_at = *tdesc.lptdesc;

  // if the referenced type is userdefined, gets its definition
  bool userdef = false;

  if(pointed_at.vt == VT_USERDEFINED)
  {
    tCOMPtr<ITypeInfo> referenced_type;
    CHK_COM_CODE(typeinfo->GetRefTypeInfo(pointed_at.hreftype, &referenced_type));
    tTypeAttrHolder typeattr(referenced_type);
    CHK_COM_CODE(referenced_type->GetTypeAttr(typeattr.out()));

    if(typeattr.get()->typekind == TKIND_ALIAS)
    {
      // Keep the pointer level and the alias metadata owner until the target
      // is resolved. An interface alias and an interface-pointer alias differ.
      TYPEDESC alias_pointer = tdesc;
      alias_pointer.lptdesc = &typeattr.get()->tdescAlias;
      return processPTR(referenced_type, alias_pointer);
    }

    userdef = true;
    pointed_at = processUSERDEFINED(typeinfo, pointed_at);
  }

  if(userdef == true &&
     (pointed_at.vt == VT_DISPATCH || pointed_at.vt == VT_UNKNOWN))
  {
    // does nothing, because it's a VT_USERDEFINED TYPEDESC that
    // describes an interface that inherits from IDispatch.
    // Pointers (that is, single indirection) to IDispatch
    // are always VT_DISPATCH.
  }
  else if(pointed_at.vt == VT_PTR)
  {
    // continues indirection
    pointed_at = processPTR(typeinfo, pointed_at);

    // We arrive here if the TYPEDESC describes a
    // pointer to a pointer. This only happens
    // when we are refencing interfaces. Since
    // interfaces are always refenced as pointers,
    // it looks like a single indirection

    pointed_at.vt |= VT_BYREF;
  }
  else if(pointed_at.vt == VT_SAFEARRAY)
  {
    pointed_at = processSAFEARRAY(typeinfo, pointed_at);
    pointed_at.vt |= VT_BYREF;
  }
  else // other types under a VT_PTR are just BYREF
  {
    pointed_at.vt |= VT_BYREF;
  }

  return pointed_at;
}

TYPEDESC tLuaCOMTypeHandler::processUSERDEFINED(ITypeInfo* typeinfo,
                                                const TYPEDESC &tdesc)
{
  HRESULT hr = S_OK;
  tCOMPtr<ITypeInfo> userdef;
  TYPEDESC newtdesc;

  newtdesc.vt = 0;

  hr = typeinfo->GetRefTypeInfo(tdesc.hreftype, &userdef);

  if(FAILED(hr))
    TYPECONV_ERROR("Could not understand user-defined type");

  tTypeAttrHolder typeattr(userdef);
  hr = userdef->GetTypeAttr(typeattr.out());

  if(FAILED(hr))
    TYPECONV_ERROR("Could not understand user-defined type");

  switch(typeattr.get()->typekind)
  {
  case TKIND_ENUM:
    newtdesc.vt = VT_INT;
    break;

  case TKIND_DISPATCH:
    newtdesc.vt = VT_DISPATCH;
    break;

  case TKIND_ALIAS:
    // shouldn't arrive here: aliases must be removed via
    // processAliases()
    INTERNAL_ERROR();
    break;

  case TKIND_INTERFACE:
    newtdesc.vt = VT_UNKNOWN;
    break;

  case TKIND_UNION:
    TYPECONV_ERROR("Union type not supported!");
    break;

  case TKIND_COCLASS:
    newtdesc.vt = VT_UNKNOWN;
    break;

  case TKIND_RECORD:
    TYPECONV_ERROR("Record type not supported!");
    break;

  case TKIND_MODULE:
  case TKIND_MAX:
    TYPECONV_ERROR("TKIND_MODULE and TKIND_MAX not supported!");
    break;

  default:
    TYPECONV_ERROR("Unknown TYPEKIND on VT_USERDEFINED TYPEDESC");
    break;
  }

  return newtdesc;
}


/**
  Clears a VARIANT, releasing first the memory allocated
*/
void tLuaCOMTypeHandler::releaseVariant(VARIANTARG *pvarg, bool release_memory)
{
  if(pvarg->vt & VT_BYREF && pvarg->byref != NULL)
  {
    switch(pvarg->vt & ~VT_BYREF)
    {
    case VT_BSTR:
      SysFreeString(*pvarg->pbstrVal);
      break;

    case VT_DISPATCH:
      COM_RELEASE(*pvarg->ppdispVal);
      break;

    case VT_UNKNOWN:
      COM_RELEASE(*pvarg->ppunkVal);
      break;

    case VT_VARIANT:
      // a variant cannot contain another BYREF
      // so we just clear with VariantClear
      VariantClear(pvarg->pvarVal);
      break;

    }

    if(pvarg->vt & VT_ARRAY && *pvarg->pparray)
      SafeArrayDestroy(*pvarg->pparray);

    if(release_memory)
    {
      CoTaskMemFree(pvarg->byref);  // after CoTaskMemAlloc
      //char *p = (char*)pvarg->byref;
      //delete [] p;
      VariantClear(pvarg);
      //pvarg->byref = NULL;
      //pvarg->vt = VT_EMPTY;
    }
  }
  else
    VariantClear(pvarg);
}


/**
  Dereferences typedef's in type descriptions
*/
TYPEDESC tLuaCOMTypeHandler::processAliases(ITypeInfo* typeinfo,
                                            const TYPEDESC &tdesc)
{
  // if it's not a userdefined type, does nothing
  if(tdesc.vt != VT_USERDEFINED)
    return tdesc;

  HRESULT hr = S_OK;
  
  tCOMPtr<ITypeInfo> userdef;
  hr = typeinfo->GetRefTypeInfo(tdesc.hreftype, &userdef);

  if(FAILED(hr))
    TYPECONV_ERROR("Could not understand user-defined type");

  tTypeAttrHolder typeattr(userdef);
  hr = userdef->GetTypeAttr(typeattr.out());

  if(FAILED(hr))
    TYPECONV_ERROR("Could not understand user-defined type");

  TYPEDESC newtdesc;
  newtdesc.vt = 0;
  if(typeattr.get()->typekind == TKIND_ALIAS)
    newtdesc = processTYPEDESC(userdef, typeattr.get()->tdescAlias);
  else
    newtdesc = tdesc;

  return newtdesc;
}


TYPEDESC tLuaCOMTypeHandler::processTYPEDESC(ITypeInfo* typeinfo,
                                             TYPEDESC tdesc)
{
  // removes aliases
  tdesc = processAliases(typeinfo, tdesc);

  bool done = false;

  switch(tdesc.vt)
  {
  case VT_USERDEFINED:
    tdesc = processUSERDEFINED(typeinfo, tdesc);
    break;

  case VT_PTR:
    tdesc = processPTR(typeinfo, tdesc);
    break;

  case VT_SAFEARRAY:
    tdesc = processSAFEARRAY(typeinfo, tdesc);
  }

  CHECKPOSCOND(tdesc.vt != VT_USERDEFINED && tdesc.vt != VT_PTR);

  return tdesc;
}


/**
  Checks whether the Lua value is of tag LuaCOM_IUnknown.
*/
bool tLuaCOMTypeHandler::isIUnknown(lua_State* L, stkIndex value)
{
  lua_pushvalue(L, value);

  bool result = luaCompat_isOfType(L, MODULENAME, LCOM_IUNKNOWN_TYPENAME) != 0;

  lua_pop(L, 1);

  return result;
}


void tLuaCOMTypeHandler::pushIUnknown(lua_State* L, IUnknown *punk)
{
  luaCompat_pushTypeByName(L, MODULENAME, LCOM_IUNKNOWN_TYPENAME);
  // Acquire the wrapper's reference only after Lua owns its userdata.
  if(luaCompat_newTypedObject(L, punk))
    punk->AddRef();
}


void tLuaCOMTypeHandler::initByRefParam(VARIANTARG* pvarg, VARTYPE vt, bool alloc_memory)
{
  CHECKPRECOND(!(vt & VT_BYREF));
  VariantClear(pvarg);

  pvarg->byref = NULL;

  if(alloc_memory) {
    const long size = VariantSize(vt);

    pvarg->byref = (void *) CoTaskMemAlloc(size);
    CHKMALLOC(pvarg->byref);

    //pvarg->byref = (void *) new char[size];

    // Initializes the allocated memory
    if(vt == VT_VARIANT)
      VariantInit(pvarg->pvarVal);
    else
      memset(pvarg->byref, 0, size);
  }

  // Cleanup can inspect this value only after its storage is initialized.
  pvarg->vt = vt | VT_BYREF;
}

void tLuaCOMTypeHandler::toByRefParam(VARIANT &var_source, VARIANTARG* pvarg_dest)
{
  CHECKPARAM(pvarg_dest);
  CHECKPRECOND(!(var_source.vt & VT_BYREF));

  // if a VARIANT ByRef is expected, just copies the value
  // to the ByRef VARIANT
  if(pvarg_dest->vt == (VT_VARIANT | VT_BYREF))
  {
    // we do a hard copy, to avoid increasing the reference count
    // objects by the use of VariantCopy
    memcpy(pvarg_dest->pvarVal, &var_source, sizeof(VARIANT));
    return;
  }

  VARTYPE vt_dest = pvarg_dest->vt & ~VT_BYREF;

  // tries to convert the value to the type expected in varg_dest
  if(!(vt_dest & VT_ARRAY))
  {
    HRESULT hr =
      VariantChangeType(&var_source, &var_source, 0, vt_dest);

    CHK_COM_CODE(hr);
  }

  // clears the old contents
  releaseVariant(pvarg_dest, false);

  pvarg_dest->vt = vt_dest | VT_BYREF;

  // does a hard copy of the memory contents,
  memcpy(pvarg_dest->byref, &var_source.byref, VariantSize(vt_dest));
}

TYPEDESC tLuaCOMTypeHandler::processSAFEARRAY(ITypeInfo* typeinfo,
                                              TYPEDESC &tdesc)
{
  CHECKPRECOND(tdesc.vt == VT_SAFEARRAY);

  // continues indirection
  TYPEDESC pointed_at;
  pointed_at = *tdesc.lptdesc;
  pointed_at = processTYPEDESC(typeinfo, pointed_at);
  pointed_at.vt |= VT_ARRAY;

  return pointed_at;
}


/**
 Returns the size in bytes of a VARIANT.
*/
long tLuaCOMTypeHandler::VariantSize(VARTYPE vt)
{
  if(vt & VT_ARRAY)
  {
    return sizeof(SAFEARRAY *);
  }

  switch(vt)
  {
  case VT_I2: return 2;
  case VT_I4: return 4;
  case VT_I8: return 8;
  case VT_R4: return 4;
  case VT_R8: return 8;
  case VT_CY: return sizeof(CURRENCY);
  case VT_DATE:     return sizeof(DATE);
  case VT_BSTR:     return sizeof(BSTR);
  case VT_DISPATCH: return sizeof(IDispatch*);
  case VT_ERROR:    return sizeof(SCODE);
  case VT_BOOL:     return sizeof(VARIANT_BOOL);
  case VT_VARIANT:  return sizeof(VARIANT);
  case VT_UNKNOWN:  return sizeof(IUnknown*);
  case VT_DECIMAL:  return 16;
  case VT_UI1: case VT_I1: return 1;
  case VT_UI2: return 2;
  case VT_UI4: return 4;
  case VT_UI8: return 8;
  case VT_INT: return sizeof(int);
  case VT_UINT: return sizeof(unsigned int);
  default: TYPECONV_ERROR("Unknown type");
  }
}


/**
  Converts VARIANT to type `vt`.  Reads from `src` and writes to `dest`.
*/
void tLuaCOMTypeHandler::Coerce(VARIANTARG &dest, VARIANTARG src, VARTYPE vt)
{
  if(vt == VT_VARIANT)
  {
    return;  // do nothing. We already have a VARIANT
  }

  VARIANTARG converted;
  VariantInit(&converted);
  HRESULT hr = VariantChangeType(&converted, &src, 0, vt);
  if(FAILED(hr))
  {
    VariantClear(&converted);
    CHK_COM_CODE(hr);
  }

  VariantClear(&dest);
  dest = converted;
}
