# A module the top level includes, and a top-level def of one of its names after
# that: a bare call in a class method runs the version live at the call.
module M
  def ik = :module
  def two(a, b = 2) = [:module, a, b]
end
include M

class C
  def go = ik
  def pair(x) = two(x)
end

class D < Object
  def go = ik
end

p C.new.go, D.new.go, C.new.pair(1)
def ik = :top
p C.new.go, D.new.go, C.new.pair(1)
def two(a, b = 3) = [:top, a, b]
p C.new.pair(1)

# the copy runs with the call's self: a class method's is the class, an instance method's the instance
module S
  def sk = self.class
end
include S
class G
  def self.go = sk
  def go = sk
end
p G.go, G.new.go
def sk = :top
p G.go, G.new.go

# an inherited class method's self is the receiving class; a builtin's reopening holds its own self
module V
  def vk = self
end
include V
class H
  def self.go = vk
end
class J < H; end
class String
  def go = vk
end
p J.go, H.go, "ab".go
def vk = :top
p J.go, H.go, "ab".go

# a default spelled at the call site, and a `class << self` method, take the receiving class too
module W
  def wk = self
end
include W
class P
  def self.go(x = wk) = x
  class << self
    def sgo = wk
  end
end
class Q < P; end
p Q.go, Q.sgo
def wk = :top
p Q.go, Q.sgo

# a call in a parameter default (method, keyword, block parameter, class method) reads the version too
module Z
  def zk = :z
end
include Z
class Rr
  def m1(x = zk) = x
  def m2(x: zk) = x
  def m4 = [1].map { |a| [2].map { |b, c = zk| c } }
  def self.c1(x = zk) = x
  def self.c2 = [1].map { |a, y = zk| y }
end
class Ss < Rr; end
def zall
  p Rr.new.m1, Rr.new.m2, Rr.new.m4, Rr.c1, Ss.c1, Rr.c2, Ss.c2
end
zall
def zk = :top
zall
