# A `def K.m` in a method body defines K.m each time that body runs. A call
# that runs before the body first does reaches what K's table holds then:
# the class's earlier method, the builtin's answer, or no method at all.

class K
  def self.cm = "cm"
end
class L; end
class M; end
class N; def self.v = "first"; end

def install = (def K.cm = "installed")
def name_k = (def K.to_s = "named")
def define_l = (def L.greet = "hi")
def never = (def M.gone = "gone")
def bump = (def N.v = "again")

def greet_l
  L.greet
rescue NoMethodError => e
  e.class.name
end

def gone_m
  M.gone
rescue NoMethodError => e
  e.class.name
end

puts K.cm
puts K.to_s
puts greet_l
install
name_k
puts K.cm
puts K.to_s
define_l
puts greet_l
define_l
puts greet_l
puts gone_m

seen = []
2.times { seen << N.v; bump }
puts seen.inspect

# a module's method that a class includes, prepends or extends: each copy
# defines the method where it runs
class P; def self.via = "plain"; end
module Inc; def by_include = (def P.via = "include"); end
module Pre; def by_prepend = (def P.via = "prepend"); end
module Ext; def by_extend = (def P.via = "extend"); end
module Deep; def by_deep = (def P.via = "deep"); end
module Mid; include Deep; end
class Host
  include Inc
  prepend Pre
  extend Ext
  include Mid
end
puts P.via
Host.new.by_include
puts P.via
Host.new.by_prepend
puts P.via
Host.by_extend
puts P.via
Host.new.by_deep
puts P.via

# a class a builder call makes, whose class methods its block may define:
# a body's def does not answer before that body runs
S = Struct.new(:a) { class << self; attr_accessor :cfg; end }
S.cfg = 3
def setup_s = (def S.k = :mk)
puts S.respond_to?(:k)

# a class-side attr writer called before the body runs writes and defines
# nothing: the body's def is still unmade
class W; class << self; attr_accessor :cfg; end; end
W.cfg = 3
def setup_w = (def W.k = :wk)
puts W.respond_to?(:k)
setup_w
puts W.respond_to?(:k), W.k

# a writer a later `class << R` def replaces runs that def from then on:
# the class method its body makes is there once it has run, not before
class R
  class << self; attr_accessor :cfg; end
end
R.cfg = 3
puts R.respond_to?(:k)
class << R
  def cfg=(v)
    def R.k = :w
  end
end
puts R.respond_to?(:k)
R.cfg = 4
puts R.respond_to?(:k), R.k

# the same beside a method body's def of the name that never runs
class H
  class << self; attr_accessor :cfg; end
end
H.cfg = 3
class << H
  def cfg=(v)
    def H.k = :w
  end
end
def setup_h = (def H.k = :mk)
puts H.respond_to?(:k), H.cfg
H.cfg = 4
puts H.respond_to?(:k), H.k
