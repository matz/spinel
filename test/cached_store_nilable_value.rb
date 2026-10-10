# A value that may be nil stored into an Integer or Float array whose header
# a while loop caches: a non-nil one in range is stored in place, a nil one
# keeps the array's nil, past the end the array grows (with nils between),
# and a frozen array raises, each read back as CRuby does.
def show(tag)
  r = yield
  puts "#{tag} #{r.inspect}"
rescue => e
  puts "#{tag} #{e.class}: #{e.message}"
end

def shift_up(a, src, n)
  i = 0
  while i < n
    a[i] = src[i + 1]          # src[n] is past its end: nil
    i += 1
  end
  a
end

def put_at(a, idx, vals)
  j = 0
  while j < idx.size
    a[idx[j]] = vals[j]
    j += 1
  end
  a
end

def heap_sift(d, i)
  while i > 0
    p = (i - 1) / 2
    break if d[p] <= d[i]
    t = d[i]
    d[i] = d[p]
    d[p] = t
    i = p
  end
  d
end

show("int") { shift_up([0, 0, 0], [1, 2, 3, 4], 3) }
show("int nil") { shift_up([0, 0, 0], [1, 2, 3], 3) }
show("float") { shift_up([0.0, 0.0], [0.5, 1.5, 2.5], 2) }
show("float nil") { shift_up([0.0, 0.0], [0.5, 1.5], 2) }
show("grow") { put_at([1, 2], [4, 0], [9, 8]) }
show("nil then value") { put_at([1, 2, 3], [1, 1], [nil, 7]) }
show("float grow") { put_at([1.0], [2], [2.5]) }
show("frozen") { put_at([1, 2].freeze, [0], [5]) }
show("heap") { heap_sift([1.0, 3.0, 2.0, 0.5], 3) }
