# A module's instance-method list leaves out the helpers the compiler makes for
# a def: the no-argument sibling of an `inspect` whose parameters are all
# optional, and the method a parameter default that calls its own method moves
# into. They are no method of the module's, in a visibility line's name too.

module M
  attr_accessor :v
  private :v=
  def to_s = "m"
  def inspect(x = 1) = "i#{x}"
  def each; yield 1; end
  def e = to_enum(:each)
end
module N; include M; private :inspect; end
p M.instance_methods.sort, M.private_instance_methods(false).sort
p N.instance_methods.sort, N.private_instance_methods(false).sort

module D
  def f(a = (q = 1; q + 1)) = a
  def g(n = g(0)) = n
  private :f
end
module E; include D; public :f; end
p D.instance_methods.sort, D.private_instance_methods(false).sort
p E.instance_methods(false).sort, E.instance_methods.sort
