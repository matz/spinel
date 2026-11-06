# A parenthesized pattern is the pattern it holds: an alternative bound
# with `=>` as a hash-pattern value, an array element or the whole arm (#11079).
Binary = Struct.new(:op, :lhs, :rhs)

def describe(e)
  case e
  in Binary[op: ("==" | "!=") => op, lhs:, rhs:]
    "#{lhs} #{op} #{rhs}"
  else
    "other"
  end
end
puts describe(Binary.new("==", 1, 2))
puts describe(Binary.new("!=", 3, 4))
puts describe(Binary.new("+", 1, 2))

def kind(h)
  case h
  in {op: ((:add | :sub)) => op, n: Integer => n} then "arith #{op} #{n}"
  in {op: (:neg)} then "neg"
  else "none"
  end
end
p kind({op: :add, n: 1}), kind({op: :neg}), kind({op: :mul, n: 2})

def pair(a)
  case a
  in [(1 | 2) => x, (String) => s] then "#{x}:#{s}"
  in ((3 | 4)) then "three-or-four"
  else "no"
  end
end
p pair([2, "z"]), pair(4), pair([5, "z"])
