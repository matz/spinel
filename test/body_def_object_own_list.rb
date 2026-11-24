# `x.public_methods(false)` on an object whose class has a method a method body defines: the list
# reads the class's version cells at the call, so the survey keeps them (the own list's side, not a
# chain's), before and after the body ran, for a typed object, a boxed one and two calls in a statement.
class K
  def self.setup = (def m = :k)
  def n = 1
end
q = K.new
r = K.new
p [q.public_methods(false).sort, r.public_methods(false).sort]
K.setup
p [q.public_methods(false).sort, r.public_methods(false).sort]
class J; def j = 1; end
xs = [q, J.new]
p xs.map { |x| x.public_methods(false).sort }
fl = ARGV.empty?
p [q.public_methods(false).include?(:m), q.public_methods(fl).include?(:m), q.public_methods.include?(:m)]
