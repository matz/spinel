# A local an Integer or Float operator has taken compares without the nil
# test afterwards, since the operator raised had it been nil; after `==`,
# which answers false for a nil, the test stays.
def pick(i) = i.even? ? 3 : nil

def after_lt(i)
  a = pick(i)
  r = a < 5
  s = a > 1
  r && s
end

def after_eq(i)
  b = pick(i)
  r = b == 5
  s = b > 1
  r || s
end

p after_lt(0), after_eq(0)
p((after_lt(1) rescue :raised), (after_eq(1) rescue :raised))
