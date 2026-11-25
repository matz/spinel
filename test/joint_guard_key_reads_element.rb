# The second index reads an element the first update writes: it is read
# after that store, as Ruby reads it.
def bump(a, n)
  i = 0
  while i < n
    a[0] += 1
    a[a[0]] += 10
    i += 1
  end
end
a = [0, 0, 0, 0, 0]
bump(a, 3)
p a
