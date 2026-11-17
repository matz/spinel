# A Symbol local that a closure rewrites is not read as its first literal:
# a define_method body or a lambda in the local's own scope writes the same
# variable, so reduce and inject take the operator it holds when they run.
# Another method's local of the name (other) is another variable and does
# not count either way.

def other
  op = 5
  op
end

class Folds
  op = :+
  define_method(:to_mul) { op = :* }
  define_method(:fold) { [2, 3, 4].reduce(op) }
  f = new
  p f.fold
  f.to_mul
  p f.fold, [2, 3].inject(op)
end

op = :+
set = lambda { op = :* }
set.call
p [2, 3, 4].reduce(op), other
