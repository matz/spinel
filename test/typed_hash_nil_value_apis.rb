# A nil value stored into a typed Integer-valued Hash through a parameter is
# kept: every read, walk, copy and boxed view of the hash answers it as nil.
# (sum raises CRuby's own TypeError for the nil.)
def put(h, k, v)
  h[k] = v
  nil
end
def try(l)
  yield
rescue => e
  puts "#{l}: #{e.class}: #{e.message}"
end
x = {"a" => 1, "b" => 2}
put(x, "n", nil)
put(x, "c", 3)
p x
p x.fetch("n")
p x.fetch("a")
p x.fetch("n") { 9 }
p x.fetch("zz") { 9 }
p x.fetch("n", 8)
p x.values_at("a", "n", "zz")
p x.fetch_values("a", "n")
p x.dig("n")
x.each { |k, v| p [k, v] }
x.each_pair { |k, v| p v }
x.each_value { |v| p v }
p x.map { |k, v| v }
p x.select { |k, v| v.nil? }
p x.reject { |k, v| v.nil? }
p x.filter_map { |k, v| v && v * 2 }
p x.sort_by { |k, v| v.to_i }
p x.min_by { |k, v| v.to_i }
p x.max_by { |k, v| v.to_i }
p x.find { |k, v| v.nil? }
p x.count { |k, v| v.nil? }
p x.any? { |k, v| v.nil? }
p x.all? { |k, v| v }
p x.sum { |k, v| v.to_i }
p x.transform_values { |v| v.to_s }
p x.transform_values { |v| v ? v + 1 : 0 }
p x.group_by { |k, v| v.nil? }
p x.partition { |k, v| v.nil? }
p x.to_h { |k, v| [k, v] }
p x.to_a, x.sort, x.first, x.first(2)
p x.invert
p x.shift
p x
p x.assoc("n"), x.rassoc(nil)
p x.flatten
p x.key(nil)
p x.merge({"n" => 5}) { |key, o, n| [o, n] }
y = {"n" => 4}
y.merge!(x) { |key, o, n| o }
p y
p x.delete("n") { 0 }
put(x, "n", nil)
p x.slice("n", "a")
p x.except("a")
p x.min_by(2) { |k, v| v.to_i }
x = {"a" => 1, "b" => 2}
put(x, "n", nil)
p x.chunk_while { |a, b| a[1] && b[1] }.to_a
p x.slice_when { |a, b| a[1].nil? }.to_a
x.each_with_index { |(k, v), i| p [k, v, i] }
p x.sort { |a, b| a[0] <=> b[0] }
p x.sum { |k, v| v.to_i }
try("values.sum") { p x.values.sum }
p x.min_by { |k, v| v.to_i }
p x.each_with_object([]) { |(k, v), acc| acc << v }
p x.to_proc.call("n")
b = [x, 1].first
p b.fetch("n")
p b["n"]
q = {1 => 10}
put(q, 2, nil)
p q, q.map { |k, v| v }, q.select { |k, v| v }, q.sort_by { |k, v| v.to_i }, q.transform_values { |v| v }
p q.invert, q.key(nil), q.to_a
