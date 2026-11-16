# A class that includes a module Object already includes (before this include)
# adds nothing to its ancestors: the module's method does not stand ahead of
# what the class's chain has of the name at that point. An include of a module
# Object does not include is placed as it was.
module N
  def nk = :n
end
module O
  def nk = :o
end
class Object
  include N
end
class D
  include O
  include N
  def go = nk
end
p D.new.go

# a superclass's own def of the name, and a module Object includes after it
class B
  def bk = :b
end
module Q
  def bk = :q
  def rk = :q
end
module R
  def rk = :r
end
class Object
  include Q
  include R
end
class E < B
  include Q
  def go = [bk, rk]
end
p E.new.go

# an alias the module makes goes with its original
module T
  def tk = :t
  alias tk2 tk
end
class Object
  include T
end
class Sup
  def tk = :sup
end
class F < Sup
  include T
end
p F.new.tk2

# Object's own def of the name, made before the include
class Object
  def ok = :obj
end
module U
  def ok = :u
end
class Object
  include U
end
class G
  include U
end
p G.new.ok

# Object's own private def of the name is not the class's public answer
module X
  def xk = :x
end
include X
class Object
  private def xk = :obj
end
class H
  include X
  def go = xk
end
p H.new.go
p((H.new.xk rescue $!.class))

# a blank slate's chain stops at BasicObject: a module Object includes is
# not among its ancestors, so its own include of the module stands ahead of
# its superclass's def
module BM
  def bf = :m
end
class Object
  include BM
end
class BB < BasicObject
  def bf = :b
end
class BC < BB
  include ::BM
end
p BC.new.bf
