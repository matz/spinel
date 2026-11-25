# A versioned loop (hcv_end) whose text is near the size it versions up to:
# the size counts the code, not the line map's #line lines, so the C is
# the same with and without --no-line-map (line-map-check).
def dot(a, b)
  s = 0
  i = 0
  while i < a.length
    s += a[i] * b[i] + 0
    s += a[i] * b[i] + 1
    s += a[i] * b[i] + 2
    s += a[i] * b[i] + 3
    s += a[i] * b[i] + 4
    s += a[i] * b[i] + 5
    s += a[i] * b[i] + 6
    s += a[i] * b[i] + 7
    s += a[i] * b[i] + 8
    s += a[i] * b[i] + 9
    s += a[i] * b[i] + 10
    s += a[i] * b[i] + 11
    s += a[i] * b[i] + 12
    s += a[i] * b[i] + 13
    s += a[i] * b[i] + 14
    s += a[i] * b[i] + 15
    s += a[i] * b[i] + 16
    s += a[i] * b[i] + 17
    s += a[i] * b[i] + 18
    s += a[i] * b[i] + 19
    s += a[i] * b[i] + 20
    s += a[i] * b[i] + 21
    s += a[i] * b[i] + 22
    s += a[i] * b[i] + 23
    s += a[i] * b[i] + 24
    s += a[i] * b[i] + 25
    s += a[i] * b[i] + 26
    i += 1
  end
  s
end
p dot([1, 2, 3], [4, 5, 6])
