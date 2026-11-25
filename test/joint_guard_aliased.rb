# Two locals that name one array, and a right-hand side that reads an
# element the first update wrote: each update sees the one before it.
def upd(a, b, n)
  i = 0
  while i < n
    a[i] += 1
    b[i] += 2
    a[i + 1] += a[i]
    i += 1
  end
end
x = [0, 0, 0, 0]
upd(x, x, 3)
p x
