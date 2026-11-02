# A NilClass method that yields, called on a receiver with an effect: the
# receiver runs before the arguments, as in CRuby. Passed as the proc
# form's self it ran in the order the C compiler picked, after them.
class NilClass
  def f(x)
    yield x
    nil
  end
end
def r; print "r"; nil; end
def a; print "a"; 1; end
r.f(a) { |v| puts v }
x = nil
x.f(a) { |v| puts v }
