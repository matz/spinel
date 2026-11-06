# `x + 1` on an Array (or an Array subclass) raises TypeError: no implicit
# conversion of Integer into Array. In the arm of a conditional whose other
# arm is an Integer, or as a method's return value, the arm never returns.
class SubA < Array; end

x = SubA[1, 2]
p(if x.is_a?(Integer) then x + 1 else x.sum end)
p(x.is_a?(Integer) ? x - 1 : x.size)
p(x.is_a?(Integer) ? x * 2 : x.first)
p(x.is_a?(String) ? x + "a" : x.size)
p(x.is_a?(Integer) ? x + true : 0)
p(x.is_a?(Integer) ? x | 1 : x.last)

y = [1, 2]
p(y.is_a?(Integer) ? y + 1 : y.sum)

def f(v) = v.is_a?(Integer) ? v + 1 : v.sum
p f(SubA[3, 4])

def g(v)
  return v + 1 if v.is_a?(Integer)
  v.size
end
p g(SubA[5])

r = case x when Integer then x + 1 else x.sum end
p r

p((x + 1 rescue $!.message))
p((x - nil rescue $!.message))
p((y + "s" rescue $!.message))
