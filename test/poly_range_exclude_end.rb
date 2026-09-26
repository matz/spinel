# Range#exclude_end? on a value that is an Integer or a Range, known only at
# run time: begin and end had a boxed arm and exclude_end? had none, so it
# raised NoMethodError for a Range (#5095).

def f(i)
  return [i.begin, i.end, i.exclude_end?] if i.is_a?(Range)
  i
end
p f(1)
p f(1..2)
p f(1...3)

def g(r) = r.exclude_end?
p g([1.0..2.0, 0][0])
p g([1.0...2.0, 0][0])
p g(["a".."c", 0][0])
p g(["a"..."c", 0][0])
p((g([5, 1..2][0]) rescue "NoMethodError"))
