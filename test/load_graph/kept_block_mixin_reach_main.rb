# A call in a block a builtin keeps (a Hash's default block, an Enumerator's, a
# Proc.new body, a define_method body) meets a method a module brings after
# the block was made: prepended, extended, or a method defined later. Whether
# the program reaches that version is asked of the block as the call plan asks
# it, over its window.
module P1; def m = "pre"; end
module E1; def m = "ext"; end
module P2; def m = "pre2"; end
class A; def m = "own"; end
class Base; def self.m = "own"; end
class B < Base; end
class C; def m = "own"; end
class D; end
h = Hash.new { |_, k| A.new.m }
e = Enumerator.new { |y| y << B.m }
pr = Proc.new { [C.new.m, C.new.m] }
class D; define_method(:go) { [D.new.m, (D.new.m rescue $!.class)] }; end
p [h[1], e.first, pr.call]
class A; prepend P1; end
class B; extend E1; end
class C; prepend P2; end
class D; def m = "late"; end
p [h[2], e.first, pr.call, D.new.go]
