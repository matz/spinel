# Pidigits - extract digits of pi using Gosper's series
# Tests bigint arithmetic (auto-promoted from loop multiplication)

n = Integer(ARGV[0] || 27)
q = 1
r = 0
t = 1
k = 0
i = 0
result = ""
while i < n
  k = k + 1
  b = 2 * k + 1
  nq = q * k
  nr = (2 * q + r) * b
  nt = t * b
  q = nq
  r = nr
  t = nt
  while (q * 3 + r) / t == (q * 4 + r) / t
    d = (q * 3 + r) / t
    result = result + d.to_s
    r = 10 * (r - d * t)
    q = q * 10
    i = i + 1
    if i >= n
      break
    end
  end
end
puts result
