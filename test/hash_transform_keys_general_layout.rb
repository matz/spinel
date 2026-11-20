def rename_keys(hash)
  hash.transform_keys { |key| key.length }
end

values = { "a" => 11, "bb" => 12, "ccc" => 13 }
values["a"] = nil
renamed = rename_keys(values)
p renamed
p renamed.transform_values { |value| value.nil? ? -9 : value }
p values.transform_keys({ "a" => 88, "bb" => 42 }) { |key| key.length }

first = +"first"
strings = { "a" => first, "bb" => +"second" }
mixed = strings.transform_keys do |key|
  next 0.25 if key.length == 1
  key.length
end
mixed[0.25] << "!"
p mixed, strings, mixed[0.25].equal?(first)

nullable = { "a" => 1, "b" => 2 }
p nullable.transform_keys { |key| key.index("a") }
p nullable.transform_keys { |key| next 0 if key == "a"; nil }
p nullable.transform_keys { |key| next key.index("a") if key == "b"; 0 }
