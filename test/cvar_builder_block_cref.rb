# A class variable is looked up through the cref, the class or module body
# the code is written in. The block of Struct.new, Data.define, Class.new
# and Module.new takes the new class as self but opens no cref, so a class
# variable written in it, or in a method defined in it, is the enclosing
# class's (or module's), not the built class's.

class Host
  @@count = 0
  S = Struct.new(:a) do
    @@h = {"x" => 1}
    @@count += 10
    def put = (@@h[:b] = 2.5)
    def all = @@h
    def initialize(*)
      super
      @@count += 1
    end
  end
  D = Data.define(:k) do
    @@tag = "d"
    def tag = @@tag + k.to_s
  end
  K = Class.new do
    @@n ||= 0
    @@n += 5
    def inc = (@@n += 1)
  end
  M = Module.new do
    @@m = :mod
    def self.m = @@m
  end
  # a reopening's body is the built class's own cref
  class S
    @@own = "s"
    def own = @@own
  end
  def self.count = @@count
  def self.h = @@h
  def self.tag = @@tag
  def self.n = @@n
  def self.m = @@m
end

s = Host::S.new(1)
s.put
p s.all, Host.h
p Host.count, s.own
p Host::D.new(k: 3).tag, Host.tag
Host::K.new.inc
p Host.n, Host::M.m, Host.m
p Host.class_variables.sort

# a subclass of the enclosing class shares the variable
class Sub < Host
  def self.h = @@h
end
p Sub.h.size

# in a module, and two classes deep
module Box
  S = Struct.new(:a) do
    @@x, @@y = 2, "y"
    def xy = [@@x * a, @@y]
  end
  def self.xy = [@@x, @@y]
end
p Box::S.new(5).xy, Box.xy

class Outer
  class Inner
    S = Struct.new(:a) do
      @@d = 1
      def d = @@d + a
    end
    def self.d = @@d
  end
end
p Outer::Inner::S.new(1).d, Outer::Inner.d

# a class method and a singleton class body in the block, a builder in a
# method body, and two builders sharing one variable
class Cfg
  @@base = 100
  S = Struct.new(:a) do
    def self.base = @@base
    class << self
      def base2 = @@base + 1
    end
  end
  A = Struct.new(:a) do
    def set = (@@shared = 2)
  end
  B = Struct.new(:b) do
    def get = @@shared
  end
  def self.make
    Struct.new(:a) do
      def base3 = @@base + 2
    end
  end
end
Cfg::A.new(0).set
p Cfg::S.base, Cfg::S.base2, Cfg.make.new(0).base3, Cfg::B.new(0).get

# an anonymous builder's block (held in an Array), and a Module.new block's
# method copied into the class that includes it
class Anon
  @@a = 5
  LIST = [Class.new { def get = @@a }]
end
p Anon::LIST[0].new.get

class Lib
  @@m = "lib"
  M = Module.new do
    def m = @@m
  end
end
class User
  include Lib::M
end
p User.new.m

# a module's method copied into each includer looks up from the module
module Counter
  @@hits = 0
  def hit = (@@hits += 1)
end
module Wrap
  include Counter
  def hits = @@hits
end
class Page
  include Wrap
end
class Post
  include Counter
end
Page.new.hit
Post.new.hit
p Page.new.hits

# a plain class body's variable, passed to a class method that stores
# another key type, and appended another element type
class Plain
  def self.put(h) = (h[:k] = 1.5)
  @@h = {"a" => 1}
  put(@@h)
  @@a = [1]
  @@a << "s"
  def self.h = @@h
  def self.a = @@a
end
p Plain.h, Plain.a

# a builder block's multiple assignment into a variable an ||= leaves
# nullable, and a nil element into it
class Tup
  @@x ||= 1
  Struct.new(:a) { @@x, y = 2, 3 }
  def self.x = @@x
  @@n ||= 1
  Struct.new(:a) { @@n, z = nil, 3 }
  def self.n = @@n
end
p Tup.x, Tup.n

# an anonymous Class.new block's variable is the enclosing class's only:
# the new class's own reflection does not find it
class AnonReg
  k = Class.new do
    @@r = 1
    def self.g = class_variable_get(:@@r)
  end
  begin
    k.g
  rescue NameError => e
    p e.name
  end
  def self.r = @@r
end
p AnonReg.r
