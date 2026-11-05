# A Method of an alias is owned by the class or module the alias was made
# in, on its singleton for an alias in a `class << self`, whatever class or
# module the aliased method came from.
module K
  def foo = "K"
  alias kk foo
end
module M
  def foo = "M+" + super
end
class C
  include K
  alias_method :bar, :foo
end
class D
  include K
  alias_method :bar, :foo
  prepend M
end
class E < C; end
p C.new.method(:bar).owner, D.new.method(:bar).owner, D.new.method(:bar).call
p E.new.method(:bar).owner, C.instance_method(:bar).owner, C.new.method(:kk).owner
module V
  def v = "V"
end
class F
  extend V
  class << self
    alias vv v
  end
  def self.w = "W"
  class << self
    alias ww w
  end
end
class G < F; end
p F.method(:vv).owner, F.method(:ww).owner, G.method(:ww).owner
module X
  extend self
  def x = "X"
  alias xx x
end
p X.method(:xx).owner
class H
  def h = "H"
  alias hh h
  def m = method(:hh).owner
end
p H.new.m
