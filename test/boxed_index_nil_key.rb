# An Integer key that may be nil, read or written through a receiver that is
# boxed (an Array or nil, a Hash): CRuby calls [] on the receiver before the
# key is converted, so a nil receiver's NoMethodError comes ahead of a nil
# key's TypeError, an Array answers the TypeError, and a Hash takes nil as a
# key. The key used to be unwrapped ahead of the call, which raised the
# TypeError first and refused a Hash's nil key.
def show(tag)
  r = yield
  puts "#{tag} #{r.inspect}"
rescue => e
  puts "#{tag} #{e.class}: #{e.message}"
end

def look(h, s) = h[s.index("b")]

def put(h, s, v)
  h[s.index("b")] = v
  h
end

def bump(a, s)
  a[s.index("b")] += 1
  a
end

def memo(a, s)
  a[s.index("b")] ||= 7
end

def sum_at(present, s, n)
  a = present ? [10, 20, 30] : nil
  k = s.index("b")
  t = 0
  j = 0
  while j < n
    t += a[k]
    j += 1
  end
  t
end

show("hash key") { look({1 => :one, nil => :none}, "abc") }
show("hash nil key") { look({1 => :one, nil => :none}, "xyz") }
show("array") { look([5, 6, 7], "abc") }
show("array nil key") { look([5, 6, 7], "xyz") }
show("nil receiver, nil key") { look(nil, "xyz") }
show("put hash nil key") { put({1 => :one}, "xyz", :n) }
show("put array") { put([1, 2, 3], "abc", 9) }
show("put array nil key") { put([1, 2, 3], "xyz", 9) }
show("put nil receiver, nil key") { put(nil, "xyz", 9) }
show("bump") { bump([1, 2, 3], "abc") }
show("bump nil key") { bump([1, 2, 3], "xyz") }
show("bump nil receiver, nil key") { bump(nil, "xyz") }
show("memo hash nil key") { memo({}, "xyz") }
show("memo array nil key") { memo([nil, nil], "xyz") }
show("loop") { sum_at(true, "abc", 2) }
show("loop nil key") { sum_at(true, "xyz", 2) }
show("loop nil receiver") { sum_at(false, "abc", 2) }
show("loop nil receiver, nil key") { sum_at(false, "xyz", 2) }
show("loop no pass") { sum_at(false, "xyz", 0) }
