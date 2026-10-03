# Array#+ / - / & / | with an operand that can never be an Array raise
# CRuby's TypeError "no implicit conversion of X into Array". The arms took
# only an Array, so the call fell through and raised NoMethodError for a
# method Array has, which a `rescue TypeError` did not catch.
def t
  p yield
rescue TypeError => e
  puts "#{e.class}: #{e.message}"
end

t { [1, 2] + nil }
t { [1, 2] - 5 }
t { [1, 2] & "s" }
t { [1, 2] | 1.5 }
t { [1, 2] + :sym }
t { [1, 2] - true }
t { [1, 2] & false }
t { [1] + (1..2) }
t { [1] | { a: 1 } }
t { %w[a b] + 3 }
t { [1.5] - nil }

def add(list, extra) = list + extra
t { add([1, 2], 5) }
# the same slot holding nil names nil, as CRuby does, not its static class
t { add([1, 2], nil) }
def sub(list, extra) = list - extra
t { sub([1.5], 2.5) }
t { sub([1.5], nil) }
def cat(list, extra) = list | extra
t { cat([1], "s") }
t { cat([1], nil) }

t { [1, 2] + [3] }
t { [1, 2] - [2] }
