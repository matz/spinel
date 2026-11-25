# spinel: int64 -- assumes a 64-bit Integer (values or arithmetic past 2^31); not run on a 32-bit target
# The second index overflows: the first update has been stored when the
# error is raised, as in Ruby.
def step(h, j, i)
  k = 0
  while k < 1
    h[j] += 1.0
    h[i + 1] += 2.0
    k += 1
  end
end
h = [0.0, 0.0, 0.0]
step(h, 0, 1)
p h
begin
  step(h, 1, 9223372036854775807)
rescue RangeError, IndexError => e
  p e.class
end
p h
