# A pattern guard whose operand is kept in a temp across a call that
# allocates reads the bindings of its own arm: the temp is filled after the
# pattern has bound, not ahead of the whole case.
def s(x); x; end
def mk(n); [n, n + 1]; end

r = case s([1, 2])
    in [a, b] if a + mk(b).length == 3 then "sum3"
    else "no"
    end
p r                                       # "sum3"

r = case s([5, 2])
    in [a, b] if a + mk(b).length == 3 then "sum3"
    else "no"
    end
p r                                       # "no"

def t(v)
  case v
  in [x, y] if x.to_s + y.to_s == "12" then "twelve"
  in [x, y] if [x, y].sum + [y].length == 8 then "eight"
  in [x, y] unless x.to_s + y.to_s == "99" then "neither"
  else "other"
  end
end
p t([1, 2])                               # "twelve"
p t([3, 4])                               # "eight"
p t([7, 7])                               # "neither"
p t([9, 9])                               # "other"

# a hash pattern, the guard built from both of its bindings
def h(v)
  case v
  in { name: String => nm, age: Integer => ag } if nm + ag.to_s == "x5" then [nm, ag]
  else "no"
  end
end
p h({ name: "x", age: 5 })                # ["x", 5]
p h({ name: "y", age: 5 })                # "no"
