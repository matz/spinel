# A loop that holds a typed Array's header stores in place only while the
# array is not frozen: a store into a frozen Integer or Float Array raises
# FrozenError, whether the array was frozen before the loop or by a call in
# it, through each cached store form -- a plain store, a value that can be
# nil, and an op-assign of a Float local.
def store_ints(a, n)
  i = 0
  while i < n
    a[i] = i * 2
    i += 1
  end
  a
end

def store_floats(a, n)
  i = 0
  while i < n
    a[i] = i * 0.5
    i += 1
  end
  a
end

def store_maybe(a, n)
  i = 0
  while i < n
    v = i == 1 ? nil : i + 3
    a[i] = v
    i += 1
  end
  a
end

def add_local(a, r)
  i = 0
  while i < a.length
    a[i] += r
    i += 1
  end
  a
end

def freeze_midway(a, at)
  i = 0
  while i < a.length
    a.freeze if i == at
    a[i] = 7
    i += 1
  end
  a
end

def attempt(label)
  print label, ": "
  p yield
rescue FrozenError => e
  puts "FrozenError #{e.message}"
end

attempt("ints") { store_ints([0, 0, 0], 3) }
attempt("ints frozen") { store_ints([0, 0, 0].freeze, 3) }
attempt("floats") { store_floats([0.0, 0.0], 2) }
attempt("floats frozen") { store_floats([0.0, 0.0].freeze, 2) }
attempt("maybe nil") { store_maybe([1, 2, 3], 3) }
attempt("maybe nil frozen") { store_maybe([1, 2, 3].freeze, 3) }
attempt("add local") { add_local([1.0, 2.0], 0.25) }
attempt("add local frozen") { add_local([1.0, 2.0].freeze, 0.25) }
kept = [1, 2, 3, 4]
attempt("freeze midway") { freeze_midway(kept, 2) }
p kept
