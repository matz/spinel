# A return, break or next as the right operand of || / && answers the left
# and jumps otherwise (#3777). In parentheses -- `(v = h[k]) || (return x)`,
# Liquid's tablerow tag -- the jump was not recognized and the expression
# was refused.

def f(h, k)
  (v = h[k]) || (return "none")
  "got #{v}"
end
p f({ "a" => 1 }, "a")
p f({ "a" => 1 }, "b")
def g(x)
  x || return
  x * 2
end
p g(3)
p g(nil)
def h(x) = (x && (return :early)) || :late
p h(1)
p h(nil)

r = [1, nil, 3].map { |x| (x && x * 10) || (next :skip) }
p r

def first_truthy(list)
  i = 0
  found = nil
  while i < list.size
    (found = list[i]) && (break)
    i += 1
  end
  [i, found]
end
p first_truthy([nil, false, "x", "y"])

def nested(x)
  ((x)) || ((return :none))
  :some
end
p nested(1)
p nested(nil)

# in value position, and a `next` in parentheses keeps leaving the block
def nil_or(v)
  x = v.nil? || (return "nil")
  x
end
p nil_or(nil), nil_or(1)

def big(v)
  x = (v > 1) && (return :big)
  x.inspect
end
p big(1), big(2)

def pair(n) = [n > 0 && (return 1), 5]
p pair(1), pair(0)

p [1, 2, 3].map { |v| x = (v == 2 && (next :two)); x.inspect }
p [1, 2].map { |v| "#{v == 1 && (next "one")}" }
