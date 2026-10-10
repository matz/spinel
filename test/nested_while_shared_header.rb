# A loop with a nested while keeps its arrays' headers across both: the inner
# loop reads through the outer loop's cache, and a store in the inner loop
# that grows an array (or gives it a nil) refreshes the cache the outer loop
# reads after it.

def grow(a, n)
  out = []
  i = 0
  while i < n
    j = 0
    while j < 2
      a[a.size + j] = a[i] + j if i == 1
      j += 1
    end
    out << a[i] + a.size
    i += 1
  end
  [out, a]
end

def holes(a)
  i = 0
  t = 0
  while i < a.size
    k = 0
    while k < 2
      a[i + k + 4] = [7][i + 5] if i == 0 && k == 1
      k += 1
    end
    v = a[i]
    t += v.nil? ? 100 : v
    i += 1
  end
  [t, a]
end

p grow([1, 2, 3], 3)
p holes([1, 2, 3])
