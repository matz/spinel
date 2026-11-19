# Versions an exit handler makes are made when it runs, after the load, and
# the handlers run in CRuby's order: the last registered first, an END on the
# same stack, a handler a handler registers right after it. A call in the
# load, in a handler or in a method either runs reads the version of its
# moment.
class A; def baz = "baz"; end
class B; def baz = "baz"; end
class C; def baz = "baz"; end
class D; def baz = "baz"; end
class E; def baz = "baz"; end

def seen(k, tag) = puts("#{tag} #{k.name}: #{k.new.baz} #{k.new.respond_to?(:nw)} #{k.method_defined?(:nw)}")
def tell(tag)
  [A, B, C, D, E].each { |k| seen(k, tag) }
end

# A: the handler that redefines is registered last, so it runs first
at_exit { seen(A, :a_second) }
at_exit do
  class A
    def baz = "new"
    def nw = "nw"
  end
  nil
end

# B: registered first, so it runs after the handler that reads
at_exit do
  class B
    def baz = "new"
    def nw = "nw"
  end
  nil
end
at_exit { seen(B, :b_first) }

# C: an END and an at_exit share the stack
END { seen(C, :c_end) }
at_exit do
  class C
    def baz = "new"
    def nw = "nw"
  end
  nil
end

# D: a handler registered inside a handler runs right after it
at_exit do
  at_exit { seen(D, :d_inner) }
  seen(D, :d_outer)
  class D
    def baz = "new"
    def nw = "nw"
  end
  seen(D, :d_after_def)
end

# E: a method only the handlers call, and its own reads
class E
  def self.redefine
    def baz = "new"
    def nw = "nw"
  end
end
at_exit { seen(E, :e_first_registered) }
at_exit { E.redefine; seen(E, :e_last_registered) }

tell(:load)
