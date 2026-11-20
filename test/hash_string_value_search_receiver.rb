def integer_keys
  p :integer_receiver
  {7 => "kept"}
end

def string_keys
  p :string_receiver
  {"key" => "kept"}
end

def nil_needle
  p :needle
  GC.start
  other = {41 => "other"}
  other[42] = ENV["SPINEL_TEST_HASH_SEARCH_UNSET"]
  GC.start
  nil
end

ENV.delete("SPINEL_TEST_HASH_SEARCH_UNSET")
p integer_keys.has_value?(nil_needle)
p string_keys.value?(nil_needle)
