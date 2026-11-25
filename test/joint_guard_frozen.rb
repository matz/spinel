# A frozen array: the first update raises, and nothing is stored.
def upd(a, n)
  i = 0
  while i < n
    a[i] += 1
    a[i + 1] += 2
    i += 1
  end
end
a = [1, 2, 3, 4].freeze
begin
  upd(a, 2)
rescue => e
  p e.class
end
p a
