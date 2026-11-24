# A def a class_eval block makes under a condition is there only where the condition held. The load record follows a
# class_eval, class_exec, module_eval or module_exec with a literal block, which runs its block once with the class for
# self, instead of holding the call (and so the class it is made in) opaque; the def then has a cell of its own, as one
# directly under an `if` has, and a reflection or a call of its name answers from the cell, not from a def that never ran.
# (No list of methods and no send of the name appears here: those read the final tables, as they do for a def directly
# under an `if`.)
class A
  def tail = :tail
  def self.mk(flag)
    A.class_eval { def zz_a; :zz_a; end } if flag
  end
end
A.mk(false)
a = A.new
p [a.respond_to?(:zz_a), A.method_defined?(:zz_a), (a.zz_a rescue $!.class)]
A.mk(true)
p [a.respond_to?(:zz_a), A.method_defined?(:zz_a), (a.zz_a rescue $!.class)]

# the class a subclass inherits from, and a module a class includes
class P
  def tail = :tail
  def self.mk(flag)
    P.class_exec { def zz_p; :zz_p; end } if flag
  end
end
class Q < P
end
P.mk(false)
q = Q.new
p [q.respond_to?(:zz_p), Q.method_defined?(:zz_p), (q.zz_p rescue $!.class)]
P.mk(true)
p [q.respond_to?(:zz_p), Q.method_defined?(:zz_p), (q.zz_p rescue $!.class)]

module M
  def tail = :tail
  def self.mk(flag)
    M.module_eval { def zz_m; :zz_m; end } if flag
  end
end
class B
  include M
end
M.mk(false)
b = B.new
p [b.respond_to?(:zz_m), B.method_defined?(:zz_m), (b.zz_m rescue $!.class)]
M.mk(true)
p [b.respond_to?(:zz_m), B.method_defined?(:zz_m), (b.zz_m rescue $!.class)]

# the block with two defs, and its call under a second condition
class D
  def tail = :tail
  def self.mk(flag, other)
    if other
      D.class_exec { def zz_e; :zz_e; end; def zz_d; :zz_d; end } if flag
    end
  end
end
D.mk(true, false)
d = D.new
p [d.respond_to?(:zz_d), D.method_defined?(:zz_d), (d.zz_d rescue $!.class)]
D.mk(false, true)
p [d.respond_to?(:zz_d), D.method_defined?(:zz_d), (d.zz_d rescue $!.class)]
D.mk(true, true)
p [d.respond_to?(:zz_d), D.method_defined?(:zz_d), (d.zz_d rescue $!.class)]

# two blocks in one method, each under its own condition: only the first ran
class E
  def tail = :tail
  def self.mk(fa, fb)
    E.class_eval { def zz_f; :zz_f; end } if fa
    E.class_eval { def zz_g; :zz_g; end } if fb
  end
end
E.mk(true, false)
e = E.new
p [e.respond_to?(:zz_f), e.respond_to?(:zz_g), (e.zz_f rescue $!.class), (e.zz_g rescue $!.class)]
