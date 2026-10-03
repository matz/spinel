# A Hash walk's Enumerator with with_index: the walk's own answer, with the
# index beside each pair (or value)
h = { "a" => 1, "b" => 2, "c" => 3 }
p h.select.with_index { |(k, v), i| i == 0 }
p h.select.with_index { |kv, i| i.odd? }
p h.filter.with_index(1) { |(k, v), i| i == 3 }
p h.reject.with_index { |(k, v), i| i == 0 }
p h.filter_map.with_index { |(k, v), i| "#{k}#{i}" if i > 0 }
h.each_pair.with_index(10) { |kv, i| p [kv, i] }
p h.transform_values.with_index { |v, i| v * 10 + i }
p h.transform_keys.with_index { |k, i| "#{k}#{i}" }
p [10, 20, 30].filter_map.with_index { |x, i| x if i > 0 }
n = 0
p h.select.with_index { |(k, v), i| n += 1; next false if i == 1; true }, n

p h.select.each_with_index { |(k, v), i| i.odd? }
p h.reject.each_with_index { |kv, i| kv[1] == 2 || i == 0 }

# the transforms' Enumerator chains answer the transformed Hash
p h.transform_values.each { |v| v * 10 }
p h.transform_keys.each_with_index { |k, i| "#{k}#{i}" }
p h.transform_values.with_index(10) { |v, i| v * i }

# a block that breaks, or whose body rescues
p h.select.with_index { |(k, v), i| break 42 if i == 1; true }
r = h.transform_values.each do |v|
  Integer("x")
rescue ArgumentError
  v - 1
end
p r

# with_index(nil) counts from 0
p h.select.with_index(nil) { |(k, v), i| i == 0 }
