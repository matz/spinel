# The second index is one past the end: its element is nil, so the update
# raises after the first one stored.
def upd(a, n)
  i = 0
  while i < n
    a[i] += 1.5
    a[i + 1] += 2.0
    i += 1
  end
end
a = [0.0, 0.0, 0.0]
begin
  upd(a, 3)
rescue NoMethodError => e
  p e.class
end
p a
