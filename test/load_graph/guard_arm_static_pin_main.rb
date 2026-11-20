# An entry's arm, and the else arm that runs the method behind the layers,
# call their own method for every receiver of its class or a subclass. A
# subclass that defines the name too, with a prepend of its own that hides
# its def under the same hidden name, is no target of them: the receiver's
# class does not pick the arm's method. Here a later prepend into A puts
# a guard entry in front of A's def, whose else arm runs A's def where B's
# is not its target, whatever a B or C receiver's super asked it.
module L1; def j(x) = [:l1, x, super]; end
module P1; def j(x) = [:p1, x, super]; end
class A; def j(x) = [:a, x]; end
class B < A; def j(x) = [:b, x, super]; end
class C < B; end
class B; prepend P1; end
p [B.new.j(1), C.new.j(2), A.new.j(3)]
class A; prepend L1; end
p [B.new.j(1), C.new.j(2), A.new.j(3)]

# with a block, through a keyword and a default
module L2; def k(a, b = 5, n: 1, &blk) = [:l2, a, b, n, super]; end
module P2; def k(a, b = 5, n: 1, &blk) = [:p2, a, b, n, super]; end
class A; def k(a, b = 5, n: 1) = [:ak, a, b, n, (yield if block_given?)]; end
class B; def k(a, b = 5, n: 1, &blk) = [:bk, a, b, n, super]; end
class B; prepend P2; end
p [B.new.k(1) { :blk }, C.new.k(2, 3, n: 4) { :c }, A.new.k(0) { :a }]
class A; prepend L2; end
p [B.new.k(1) { :blk }, C.new.k(2, 3, n: 4) { :c }, A.new.k(0) { :a }]

# the class side
module SP; def s = [:sp, super]; end
module SL; def s = [:sl, super]; end
class A; def self.s = [:as]; end
class B; def self.s = [:bs, super]; end
class B; singleton_class.prepend SP; end
p [B.s, C.s, A.s]
class A; singleton_class.prepend SL; end
p [B.s, C.s, A.s]
