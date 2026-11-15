# A public top-level def is Object's public method: an instance of a class with no
# explicit superclass answers respond_to? for it, a private one only to include_all,
# a class that makes the name private, protected or undefined, or answers respond_to?
# itself, keeps its own answer.
public def pub_tk = :pub
def priv_tk = :priv
class D; end
class E; end
class F
  def respond_to?(name, include_all = false) = false
end
class G
  private def pub_tk = 2
end
class H
  def pub_tk = 3
  private :pub_tk
end
class I
  protected def pub_tk = 4
end
class J
  undef_method :pub_tk
end
class K < G; end
p D.new.respond_to?(:pub_tk), D.new.respond_to?(:priv_tk), D.new.respond_to?(:priv_tk, true)
p D.new.respond_to?(:absent_tk)
p F.new.respond_to?(:pub_tk)
p G.new.respond_to?(:pub_tk), G.new.respond_to?(:pub_tk, true)
p H.new.respond_to?(:pub_tk), H.new.respond_to?(:pub_tk, true)
p I.new.respond_to?(:pub_tk), I.new.respond_to?(:pub_tk, true)
p J.new.respond_to?(:pub_tk)
p K.new.respond_to?(:pub_tk)

# a module Object includes, a class that includes it again, and a public top-level def of its name
module N; def nk = :n; end
include N
public def nk = :obj
class L; include N; end
p L.new.respond_to?(:nk), L.new.nk
