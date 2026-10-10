# An element of an Integer or Float array a loop caches, compared with a
# number literal (`flags[i] == 1`), compares the slot itself below the
# array's nil-free length; past it, and in a gap, the element is nil, which
# equals no number. Both arrays here hold gaps.
a = Array.new(6, 1)
a[9] = 2
f = [1.5, 2.5]
f[4] = 1.5
i = 0
n1 = n2 = n3 = n4 = 0
while i < 12
  n1 += 1 if a[i] == 1
  n2 += 1 if a[i] != 2
  n3 += 1 if f[i] == 1.5
  n4 += 1 if f[i] != 1.5
  i += 1
end
p [n1, n2, n3, n4]
