# The second index is negative: the run takes the get and the set as
# written, which count from the end.
def upd(a, n)
  i = 0
  while i < n
    a[i] += 1
    a[i - 3] += 10
    i += 1
  end
end
a = [0, 0, 0, 0, 0, 0]
upd(a, 2)
p a
f = [0.0, 0.0, 0.0, 0.0]
i = 0
while i < 2
  f[i] += 0.5
  f[i + 2] -= 0.25
  i += 1
end
p f
