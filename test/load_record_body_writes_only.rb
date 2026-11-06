# A method whose body only writes variables (an instance variable or a class
# variable), called while a class body runs, supplies no method:
# the load record still knows the version a later call meets. A method that
# calls a writer, and an initialize that writes, are no more than that. So a
# method calling the class method after a later redefinition binds each
# version the way CRuby does, and the class method's own call in the class
# body stays the first one.
# A callback the runtime makes by itself (to_s) that only writes is no more
# than that either.
class P
  def to_s = (@s = "p")
end

class A; def self.v = (@x = 1); v; end
def fa = A.v
puts fa
def A.v = 2
puts fa

class C; def self.v = (@@c = 1); v; end
def fc = C.v
puts fc
def C.v = 2
puts fc

# through a second method that writes
class D
  def self.set = (@y = 1)
  def self.v = (set; 1)
  v
end
def fd = D.v
puts fd
def D.v = 2
puts fd

# a call of new, whose initialize writes
class H
  def initialize; @a = 1; end
  def v = 1
  def self.make = new
  make
end
def fh = H.make.v
puts fh
class H; def v = 2; end
puts fh

# a counter in a module function
module M
  def self.cnt = (@n = (@n || 0) + 1)
  cnt
end
def fm = M.cnt
p fm
def M.cnt = -1
p fm

# a top-level method that writes a global
def setup = ($ready = true; 1)
setup
def fs = setup
puts fs
def setup = 2
puts fs

# (with the callback above in the program)
class J; def self.v = (@x = 1); v; end
def fj = J.v
puts fj
def J.v = 2
puts fj

# no variable at all
class E; def self.v = 1; v; end
def fe = E.v
puts fe
def E.v = 2
puts fe
