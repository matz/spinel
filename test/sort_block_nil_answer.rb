# A sort, min or max block whose answer is an Integer that may be nil:
# Float#<=> answers nil for NaN, and a block can answer nil itself. A
# number answer orders the elements; a nil one is CRuby's ArgumentError,
# "comparison of A with B failed".
def show(tag)
  r = yield
  puts "#{tag} #{r.inspect}"
rescue => e
  puts "#{tag} #{e.class}: #{e.message}"
end

def by_key(keys, a, b)
  c = keys[a] <=> keys[b]
  c == 0 ? a <=> b : c
end

keys = [3.5, 1.0, 2.0, 1.0]
nan_keys = [3.5, 0.0 / 0.0, 2.0]
show("sort") { [0, 1, 2, 3].sort { |a, b| by_key(keys, a, b) } }
show("sort nan") { [0, 1, 2].sort { |a, b| by_key(nan_keys, a, b) } }
show("min") { [0, 1, 2, 3].min { |a, b| by_key(keys, a, b) } }
show("max") { [0, 1, 2, 3].max { |a, b| by_key(keys, a, b) } }
show("max nan") { [0, 1, 2].max { |a, b| by_key(nan_keys, a, b) } }
show("sort nil answer") { [3, 1, 2].sort { |a, b| a == 2 || b == 2 ? nil : a <=> b } }
show("min nil answer") { [3, 1, 2].min { |a, b| a == 1 ? nil : a <=> b } }
show("float sort") { [2.5, 0.5, 1.5].sort { |a, b| a <=> b } }
show("float sort nan") { [2.5, 0.0 / 0.0, 1.5].sort { |a, b| a <=> b } }
