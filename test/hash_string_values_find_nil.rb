name = "SPINEL_TEST_STRING_HASH_MISSING_VALUE"
ENV.delete(name)
missing = ENV[name]

strings = {"present" => "value"}
numbers = {1 => "value"}
p [strings.value?(nil), numbers.has_value?(nil)]

strings["missing"] = missing
numbers[2] = missing
p [strings.has_value?(nil), strings.value?(missing), strings.value?("")]
p [numbers.value?(nil), numbers.has_value?(missing), numbers.has_value?("value".dup)]

strings["empty"] = ""
numbers.delete(2)
strings.delete("missing")
p [strings.value?(nil), strings.value?(""), numbers.has_value?(nil)]
