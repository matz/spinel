# A constant read that no enclosing class defines is looked up in the
# ancestors of the innermost one before the top level, as Ruby does. With the
# same name defined in two classes, both are renamed to qualified names, and
# a read inside a subclass of one of them matched neither and raised
# NameError -- or, with a top-level definition too, read that one.
class A; K = 1; end
class B; K = 2; end
class C < B; def self.x = K; def y = K; end
p C.x, C.new.y
class D < C; def self.x = K; end
p D.x
module M; K = 3; end
class E; include M; def self.x = K; end
p E.x
K = 9
class F; def self.x = K; end
p F.x
class G < B; K = 7; def self.x = K; end
p G.x
module Outer
  K = 5
  class H < B; def self.x = K; end
end
p Outer::H.x
class I < A
  include M
  def self.x = K
end
p I.x

# a superclass is resolved from where it is written, by its whole path:
# `B` inside Outer is Outer::B, and at the top level the top-level B
module Outer
  class B
    Q = :outer_b
  end
  class OC < B
    def self.x = Q
  end
end
class B
  Q = :top_b
end
class TD < B
  def self.x = Q
end
p Outer::OC.x, TD.x

# a module's own includes are searched too
module Inner
  R = :inner
end
module MidM
  include Inner
end
class RA
  R = :ra
end
class RC
  include MidM
  def self.x = R
end
p RC.x
