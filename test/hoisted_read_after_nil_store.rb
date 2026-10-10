# A loop's cached reads of an Integer / Float array take the cached element
# only while the array has no nil bitmap. A nil stored inside the loop gives
# it one, and the reads after it must see that nil (and the elements after a
# store that clears it again).

def scan(a, n)
  out = []
  i = 0
  while i < n
    a[i + 1] = nil if i == 1
    a[i + 2] = 9 if i == 3
    v = a[i]
    out.push(v.nil? ? -1 : v + 1)
    i += 1
  end
  out
end

def fscan(a, n)
  s = 0.0
  i = 0
  while i < n
    a[2] = nil if i == 0
    x = a[i]
    s += x.nil? ? 100.0 : x
    i += 1
  end
  s
end

def total(a)
  t = 0
  i = 0
  while i < a.size
    a[a.size - 1] = nil if i == 0
    t += a[i] || 1000
    i += 1
  end
  t
end

p scan([1, 2, 3, 4, 5, 6, 7], 7)
p fscan([1.5, 2.5, 3.5, 4.5], 4)
p total([1, 2, 3, 4])
begin
  b = [1, 2, 3]
  i = 0
  while i < 3
    b[1] = nil if i == 0
    puts b[i] + 1
    i += 1
  end
rescue NoMethodError => e
  puts e.message
end

# the same through the loop's header cache: a store whose value may be nil
def cscan(a, n)
  t = 0
  i = 0
  while i < n - 1
    w = i == 1 ? nil : i * 10
    a[i + 1] = w
    v = a[i]
    t = t + (v.nil? ? 1000 : v)
    i += 1
  end
  t
end

# a and b the same array: the store through a gives b its bitmap
def cscan2(a, b, n)
  t = 0
  i = 0
  while i < n - 1
    if b[i] > 0
      t = t + b[i]
    end
    a[i + 1] = i == 1 ? nil : 1
    i += 1
  end
  t
end

def cfscan(a, n)
  s = 0.0
  i = 0
  while i < n - 1
    a[i + 1] = i == 0 ? nil : 0.5
    x = a[i]
    s = s + (x.nil? ? 100.0 : x)
    i += 1
  end
  s
end

p cscan([1, 2, 3, 4, 5], 5)
p cfscan([1.5, 2.5, 3.5, 4.5], 4)
begin
  x = [5, 6, 7, 8]
  p cscan2(x, x, 4)
rescue NoMethodError => e
  puts e.message
end
