# Under --int-overflow=promote a loop's Integers are boxed; the loop still
# keeps its arrays' headers when its index and values are Integers or nil.
# Each case reads and stores through such a loop, including the paths that
# leave the cached header: a store past the end grows the array, a nil is
# stored and read back, a nil index raises, and a parameter bounds the loop.

def sieve(n)
  flags = Array.new(n, 1)
  count = 0
  i = 2
  while i < n
    if flags[i] == 1
      count = count + 1
      j = i * i
      while j < n
        flags[j] = 0
        j = j + i
      end
    end
    i = i + 1
  end
  count
end
p sieve(100)

a = [0, 0, 0]
i = 0
while i < 6
  a[i] = i * 10     # past the end from i == 3: the array grows
  i = i + 1
end
p a

b = Array.new(4, 7)
k = 0
while k < 4
  y = k + 5
  y = nil if k == 2
  b[k] = y
  k = k + 1
end
p b, b[2]

c = [1, 2, 3]
idx = nil
m = 0
while m < 3
  m = m + 1
  idx = m if m < 3
end
begin
  p c[idx], c[nil.to_i]
  idx = nil
  q = 0
  while q < 1
    c[q] = c[idx]
    q = q + 1
  end
  p c
rescue TypeError => e
  puts "TypeError: #{e.message}"
end

def bounded(lim)
  arr = Array.new(lim, 0)
  t = 0
  while t < lim
    arr[t] = t * t
    t = t + 1
  end
  arr
end
p bounded(5)
