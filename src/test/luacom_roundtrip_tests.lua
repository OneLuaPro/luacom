local luacom = require("luacom")
local config = luacom.config
assert(type(config) == "table")

local original_table_variants = luacom.TableVariants
local original_date_format = luacom.DateFormat
local original_decimal_format = luacom.DecimalFormat
local original_code_page = luacom.GetCodepage()
local original_abort_on_api_error = config.abort_on_API_error
local original_last_error = config.last_error

-- Lua 5.3 and later have an integer subtype; older versions only numbers.
local function assert_integer(value, expected)
  assert(value == expected)
  if math.type then
    assert(math.type(value) == "integer")
  end
end

local function assert_float(value, expected)
  assert(value == expected)
  if math.type then
    assert(math.type(value) == "float")
  end
end

local function roundtrip_variant(variant_type, value)
  luacom.TableVariants = true
  local result = luacom.RoundTrip({Type = variant_type, Value = value})
  -- RoundTrip returns the scalar value, even when TableVariants is enabled.
  assert(type(result) ~= "table")
  return result
end

assert(roundtrip_variant("int8", -42) == -42)
assert(roundtrip_variant("uint8", 42) == 42)

if math.type and math.maxinteger and math.maxinteger > 9007199254740992 then
  local exact_integer = 9007199254740993
  local signed_result = roundtrip_variant("int8", -exact_integer)
  local unsigned_result = roundtrip_variant("uint8", exact_integer)

  assert(signed_result == -exact_integer)
  assert(unsigned_result == exact_integer)
  assert(math.type(signed_result) == "integer")
  assert(math.type(unsigned_result) == "integer")

  local signed_array = luacom.RoundTrip({
    Type = "array of int8",
    Value = {-exact_integer, exact_integer},
  })
  local unsigned_array = luacom.RoundTrip({
    Type = "array of uint8",
    Value = {exact_integer},
  })

  assert(signed_array[1] == -exact_integer)
  assert(signed_array[2] == exact_integer)
  assert(unsigned_array[1] == exact_integer)
  assert(math.type(signed_array[1]) == "integer")
  assert(math.type(signed_array[2]) == "integer")
  assert(math.type(unsigned_array[1]) == "integer")
end

for _, value in ipairs({
  {"int1", -128, 127},
  {"uint1", 0, 255},
  {"int2", -32768, 32767},
  {"uint2", 0, 65535},
  {"int4", -2147483648, 2147483647},
  {"uint4", 0, 4294967295},
  {"int", -2147483648, 2147483647},
  {"uint", 0, 4294967295},
}) do
  assert_integer(roundtrip_variant(value[1], value[2]), value[2])
  assert_integer(roundtrip_variant(value[1], value[3]), value[3])
  assert_integer(roundtrip_variant(value[1], tostring(value[3])), value[3])
end

local integer_array = luacom.RoundTrip({Type = "array of int4", Value = {-1, 2}})
assert_integer(integer_array[1], -1)
assert_integer(integer_array[2], 2)
local unsigned_array = luacom.RoundTrip({Type = "array of uint2", Value = {0, 65535}})
assert_integer(unsigned_array[1], 0)
assert_integer(unsigned_array[2], 65535)

-- Plain Lua numbers: integers as VT_I4, as VT_R8 while a double is exact,
-- and as VT_I8 beyond; floats as VT_R8.
assert_integer(luacom.RoundTrip(5), 5)
assert_integer(luacom.RoundTrip(-2147483648), -2147483648)
assert_integer(luacom.RoundTrip(2147483647), 2147483647)
assert_float(luacom.RoundTrip(5.0), 5.0)
assert_float(luacom.RoundTrip(0.5), 0.5)
if math.type and math.maxinteger and math.maxinteger > 9007199254740992 then
  assert_float(luacom.RoundTrip(1099511627776), 1099511627776)
  assert_float(luacom.RoundTrip(9007199254740992), 9007199254740992)
  assert_integer(luacom.RoundTrip(9007199254740993), 9007199254740993)
  assert_integer(luacom.RoundTrip(math.maxinteger), math.maxinteger)
  assert_integer(luacom.RoundTrip(math.mininteger), math.mininteger)
end

for _, value in ipairs({
  {"int8", "-9223372036854775808"},
  {"int8", "9223372036854775807"},
  {"uint8", "18446744073709551615"},
}) do
  assert(tostring(roundtrip_variant(value[1], value[2])) == value[2])
end

local exact_string_array = luacom.RoundTrip({
  Type = "array of int8",
  Value = {"-9223372036854775808", "9223372036854775807"},
})
assert(tostring(exact_string_array[1]) == "-9223372036854775808")
assert(tostring(exact_string_array[2]) == "9223372036854775807")

local unsigned_string_array = luacom.RoundTrip({
  Type = "array of uint8",
  Value = {"18446744073709551615"},
})
assert(unsigned_string_array[1] == "18446744073709551615")

for _, value in ipairs({
  {"array of int1", {-128, 127}},
  {"array of uint2", {0, 65535}},
  {"array of uint4", {0, 4294967295}},
  {"array of uint", {0, 4294967295}},
  {"array of string", {"first", "second"}},
}) do
  local result = luacom.RoundTrip({Type = value[1], Value = value[2]})
  assert(#result == #value[2])
  for index, expected in ipairs(value[2]) do
    assert(result[index] == expected)
  end
end

local empty_array = luacom.RoundTrip({Type = "array", Value = {}})
assert(type(empty_array) == "table" and next(empty_array) == nil)

luacom.SetCodepage(65001)
for _, value in ipairs({"", "plain text", "A\0B", "caf\195\169"}) do
  assert(luacom.RoundTrip(value) == value)
end
assert(luacom.RoundTrip({Type = "string", Value = "typed text"}) == "typed text")
assert(luacom.RoundTrip({Type = "string", Value = "A\0B"}) == "A\0B")
luacom.SetCodepage(original_code_page)

assert(luacom.RoundTrip(true) == true)
assert(luacom.RoundTrip(false) == false)
assert(select("#", luacom.RoundTrip(nil)) == 1)
assert(luacom.RoundTrip(nil) == nil)

local success_error_sentinel = "round-trip success sentinel"
config.last_error = success_error_sentinel
assert(luacom.RoundTrip("valid with saved error") == "valid with saved error")
assert(luacom.config == config)
assert(config.last_error == success_error_sentinel)

config.abort_on_API_error = true
for _, value in ipairs({
  {"int1", 128},
  {"uint1", -1},
  {"int2", 32768},
  {"uint2", 65536},
  {"int4", 2147483648},
  {"uint4", 4294967296},
  {"int", 0.5},
  {"uint", -1},
  {"int8", "9223372036854775808"},
  {"int8", "-9223372036854775809"},
  {"uint8", "18446744073709551616"},
  {"uint8", -1},
  {"int8", "not an integer"},
  {"int8", 0.5},
}) do
  local ok, message = pcall(luacom.RoundTrip, {Type = value[1], Value = value[2]})
  assert(not ok)
  assert(type(message) == "string" and #message > 0)
  assert(luacom.config == config)
  local reported_error = config.last_error
  assert(type(reported_error) == "string" and #reported_error > 0)
  assert(luacom.RoundTrip("valid after error") == "valid after error")
  assert(config.last_error == reported_error)
end

config.abort_on_API_error = false
assert(select("#", luacom.RoundTrip({Type = "int4", Value = 0.5})) == 0)
assert(luacom.config == config)
local reported_error = config.last_error
assert(type(reported_error) == "string" and #reported_error > 0)
assert(luacom.RoundTrip("valid after reported error") == "valid after reported error")
assert(config.last_error == reported_error)
config.abort_on_API_error = original_abort_on_api_error

local date = {
  Year = 2024,
  Month = 6,
  Day = 20,
  Hour = 12,
  Minute = 1,
  Second = 1,
  Milliseconds = 499,
}

luacom.TableVariants = false
luacom.DateFormat = "table"
local date_result = luacom.RoundTrip(date)
assert(date_result.Year == date.Year)
assert(date_result.Month == date.Month)
assert(date_result.Day == date.Day)
assert(date_result.Hour == date.Hour)
assert(date_result.Minute == date.Minute)
assert(date_result.Second == date.Second)
assert(date_result.Milliseconds == date.Milliseconds)

local historical_date = {
  Year = 1899,
  Month = 12,
  Day = 29,
  Hour = 12,
  Minute = 34,
  Second = 56,
  Milliseconds = 499,
}
local historical_result = luacom.RoundTrip(historical_date)
assert(historical_result.Year == historical_date.Year)
assert(historical_result.Month == historical_date.Month)
assert(historical_result.Day == historical_date.Day)
assert(historical_result.Hour == historical_date.Hour)
assert(historical_result.Minute == historical_date.Minute)
assert(historical_result.Second == historical_date.Second)
assert(historical_result.Milliseconds == historical_date.Milliseconds)

local automation_epoch = {
  Year = 1899,
  Month = 12,
  Day = 30,
  Hour = 0,
  Minute = 0,
  Second = 0,
  Milliseconds = 0,
}
local epoch_result = luacom.RoundTrip(automation_epoch)
assert(epoch_result.Year == automation_epoch.Year)
assert(epoch_result.Month == automation_epoch.Month)
assert(epoch_result.Day == automation_epoch.Day)
assert(epoch_result.Hour == automation_epoch.Hour)
assert(epoch_result.Minute == automation_epoch.Minute)
assert(epoch_result.Second == automation_epoch.Second)
assert(epoch_result.Milliseconds == automation_epoch.Milliseconds)

local function assert_historical_milliseconds(year, milliseconds)
  local value = {
    Year = year,
    Month = 1,
    Day = 2,
    Hour = 0,
    Minute = 0,
    Second = 0,
    Milliseconds = milliseconds,
  }
  local result = luacom.RoundTrip(value)
  assert(result.Year == value.Year)
  assert(result.Month == value.Month)
  assert(result.Day == value.Day)
  assert(result.Hour == value.Hour)
  assert(result.Minute == value.Minute)
  assert(result.Second == value.Second)
  assert(result.Milliseconds == value.Milliseconds)
end

for _, year in ipairs({100, 1600}) do
  for _, milliseconds in ipairs({499, 500, 999}) do
    assert_historical_milliseconds(year, milliseconds)
  end
end

for _, field in ipairs({"Year", "Month", "Day", "DayOfWeek", "Hour", "Minute",
                        "Second", "Milliseconds"}) do
  assert(date_result[field] ~= nil)
  assert_integer(date_result[field], math.floor(date_result[field]))
end

-- The iso format does not depend on the locale and keeps the milliseconds.
luacom.DateFormat = "iso"
assert(luacom.RoundTrip(date) == "2024-06-20 12:01:01.499")
assert(luacom.RoundTrip(historical_date) == "1899-12-29 12:34:56.499")
assert(luacom.RoundTrip(automation_epoch) == "1899-12-30 00:00:00.000")
assert(luacom.RoundTrip({
  Year = 100,
  Month = 1,
  Day = 2,
  Hour = 0,
  Minute = 0,
  Second = 0,
  Milliseconds = 999,
}) == "0100-01-02 00:00:00.999")
assert(luacom.RoundTrip({
  Year = 9999,
  Month = 12,
  Day = 31,
  Hour = 23,
  Minute = 59,
  Second = 59,
  Milliseconds = 0,
}) == "9999-12-31 23:59:59.000")

-- Unknown values of DateFormat and DecimalFormat are conversion errors.
config.abort_on_API_error = true
for _, value in ipairs({
  {"DateFormat", "no_such_format", automation_epoch},
  {"DateFormat", true, automation_epoch},
  {"DecimalFormat", "no_such_format", {Type = "decimal", Value = "1"}},
  {"DecimalFormat", 1, {Type = "decimal", Value = "1"}},
}) do
  luacom[value[1]] = value[2]
  local ok, message = pcall(luacom.RoundTrip, value[3])
  assert(not ok)
  assert(type(message) == "string" and message:find(value[1], 1, true))
  luacom[value[1]] = nil
end
config.abort_on_API_error = original_abort_on_api_error

luacom.DateFormat = "string_ms_accurate"
date.Second = 59
date.Milliseconds = 499
local formatted_before_boundary = luacom.RoundTrip(date)
date.Milliseconds = 500
local formatted_after_boundary = luacom.RoundTrip(date)
assert(type(formatted_before_boundary) == "string")
assert(type(formatted_after_boundary) == "string")
assert(#formatted_before_boundary > 0)
assert(#formatted_after_boundary > 0)
assert(formatted_before_boundary ~= formatted_after_boundary)

for _, year in ipairs({100, 1600}) do
  date.Year = year
  date.Month = 1
  date.Day = 2
  date.Hour = 0
  date.Minute = 0
  date.Second = 0
  date.Milliseconds = 500
  assert(#luacom.RoundTrip(date) > 0)
  date.Milliseconds = 999
  assert(#luacom.RoundTrip(date) > 0)
end

-- DECIMAL and CURRENCY: DecimalFormat "number" (the default) converts
-- through a double, "string" gives exact text, "exact" gives integers for
-- DECIMAL values of scale 0 where they fit.
luacom.DateFormat = nil
luacom.DecimalFormat = nil
assert_float(luacom.RoundTrip({Type = "decimal", Value = "0.5"}), 0.5)
assert_float(luacom.RoundTrip({Type = "currency", Value = "12.25"}), 12.25)

luacom.DecimalFormat = "string"
for _, text in ipairs({
  "12345678901234567890.123456789",
  "-0.10",
  "79228162514264337593543950335",
  "-79228162514264337593543950335",
  "0",
  "0.0000000000000000000000000001",
  "1.000000000",
}) do
  assert(luacom.RoundTrip({Type = "decimal", Value = text}) == text)
end
assert(luacom.RoundTrip({Type = "decimal", Value = "+1.50"}) == "1.50")
assert(luacom.RoundTrip({Type = "decimal", Value = "-0"}) == "0")
assert(luacom.RoundTrip({Type = "currency", Value = "12.34"}) == "12.3400")
assert(luacom.RoundTrip({Type = "currency", Value = "-922337203685477.5808"})
       == "-922337203685477.5808")
assert(luacom.RoundTrip({Type = "currency", Value = "922337203685477.5807"})
       == "922337203685477.5807")
assert(luacom.RoundTrip({Type = "currency", Value = "5"}) == "5.0000")
if math.type then
  assert(luacom.RoundTrip({Type = "decimal", Value = 42}) == "42")
  assert(luacom.RoundTrip({Type = "currency", Value = 5}) == "5.0000")
end
if math.type and math.maxinteger and math.maxinteger > 9007199254740992 then
  assert(luacom.RoundTrip({Type = "decimal", Value = math.mininteger})
         == "-9223372036854775808")
end

luacom.DecimalFormat = "exact"
assert_integer(luacom.RoundTrip({Type = "decimal", Value = "42"}), 42)
assert_integer(luacom.RoundTrip({Type = "decimal", Value = "-42"}), -42)
assert(luacom.RoundTrip({Type = "decimal", Value = "42.0"}) == "42.0")
assert(luacom.RoundTrip({Type = "decimal", Value = "1.5"}) == "1.5")
assert(luacom.RoundTrip({Type = "currency", Value = "5"}) == "5.0000")
if math.type and math.maxinteger and math.maxinteger > 9007199254740992 then
  assert_integer(luacom.RoundTrip({Type = "decimal",
                                   Value = "-9223372036854775808"}),
                 math.mininteger)
  assert_integer(luacom.RoundTrip({Type = "decimal",
                                   Value = "9223372036854775807"}),
                 math.maxinteger)
  assert(luacom.RoundTrip({Type = "decimal", Value = "18446744073709551615"})
         == "18446744073709551615")
  assert(luacom.RoundTrip({Type = "decimal", Value = "-9223372036854775809"})
         == "-9223372036854775809")
end

config.abort_on_API_error = true
local invalid_numbers = {
  {"decimal", "79228162514264337593543950336"},
  {"decimal", "0.00000000000000000000000000001"},
  {"decimal", "1e5"},
  {"decimal", "1,5"},
  {"decimal", " 1"},
  {"decimal", "1 "},
  {"decimal", ""},
  {"decimal", "-"},
  {"decimal", ".5"},
  {"decimal", "1."},
  {"decimal", "1.2.3"},
  {"decimal", "0x10"},
  {"currency", "1.23456"},
  {"currency", "922337203685477.5808"},
  {"currency", "-922337203685477.5809"},
  {"currency", "1,5"},
}
if math.type then
  table.insert(invalid_numbers, {"currency", 922337203685478})
  table.insert(invalid_numbers, {"currency", -922337203685478})
end
for _, value in ipairs(invalid_numbers) do
  local ok, message = pcall(luacom.RoundTrip, {Type = value[1], Value = value[2]})
  assert(not ok)
  assert(type(message) == "string" and #message > 0)
end
config.abort_on_API_error = original_abort_on_api_error
luacom.DecimalFormat = nil

-- SetConversion sets the formats for one object and leaves the fields of
-- the luacom table alone. Scripting.Dictionary keeps its values unchanged.
local exact_decimal = "12345678901234567890.123456789"
local converted = assert(luacom.CreateInprocObject("Scripting.Dictionary"))
local plain = assert(luacom.CreateInprocObject("Scripting.Dictionary"))
for _, dictionary in ipairs({converted, plain}) do
  dictionary:Add("decimal", {Type = "decimal", Value = exact_decimal})
  dictionary:Add("date", {
    Year = 2024,
    Month = 6,
    Day = 20,
    Hour = 12,
    Minute = 1,
    Second = 1,
    Milliseconds = 499,
  })
  dictionary:Add("integer", 5)
end
assert_integer(plain:Item("integer"), 5)

luacom.SetConversion(converted, {DateFormat = "iso", DecimalFormat = "string"})
assert(converted:Item("decimal") == exact_decimal)
assert(converted:Item("date") == "2024-06-20 12:01:01.499")
assert(type(plain:Item("decimal")) == "number")
assert(plain:Item("date") ~= "2024-06-20 12:01:01.499")
assert(luacom.DateFormat == nil and luacom.DecimalFormat == nil)

luacom.DateFormat = "table"
assert(converted:Item("date") == "2024-06-20 12:01:01.499")
assert(type(plain:Item("date")) == "table")
luacom.DateFormat = nil

luacom.SetConversion(converted, {DecimalFormat = "exact"})
assert(converted:Item("decimal") == exact_decimal)
assert(type(converted:Item("date")) == "string")
assert(converted:Item("date") ~= "2024-06-20 12:01:01.499")

-- Invalid settings are refused and change nothing.
config.abort_on_API_error = true
for _, settings in ipairs({
  {DateFormat = "no_such_format"},
  {DecimalFormat = true},
  {NoSuchSetting = "iso"},
  {DateFormat = "iso", DecimalFormat = "no_such_format"},
}) do
  assert(not pcall(luacom.SetConversion, converted, settings))
  assert(converted:Item("decimal") == exact_decimal)
  assert(converted:Item("date") ~= "2024-06-20 12:01:01.499")
end
assert(not pcall(luacom.SetConversion, {}, {DateFormat = "iso"}))
config.abort_on_API_error = original_abort_on_api_error

luacom.SetConversion(converted)
assert(type(converted:Item("decimal")) == "number")
luacom.ReleaseComObject(converted)
luacom.ReleaseComObject(plain)

luacom.SetCodepage(original_code_page)
assert(luacom.GetCodepage() == original_code_page)

luacom.TableVariants = original_table_variants
luacom.DateFormat = original_date_format
luacom.DecimalFormat = original_decimal_format
assert(luacom.config == config)
config.abort_on_API_error = original_abort_on_api_error
config.last_error = original_last_error

print("LuaCOM round-trip tests passed")
