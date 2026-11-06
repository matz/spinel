# One statement can make versions in several histories at the same node:
# `module_function :a, :b` copies each name, `attr_accessor :v` makes a
# reader and a writer, and `attr_reader :x, :y` two readers. Each history's
# cell is written there, not only the first name's. A call in a copy's body
# that the program reaches before the reopening (here behind a condition
# that is false) and after it meets both copies; the second module copies
# the same two names twice.
module M
  module_function
  def a = 1
  def b = 2
  def c = a + b
end
def go = M.c
p go if ARGV.size > 3
module M
  def a = 10
  def b = 20
  module_function :a, :b
end
p go

module N
  def a = 1
  def b = 2
  def c = a + b
  module_function :a, :b, :c
end
p N.c
module N
  def a = 10
  def b = 20
  module_function :a, :b
end
p N.c

module O
  def a = 1
  def b = 2
  module_function :a, :b
  def self.c = a * 100 + b
end
p O.c
def O.b = 20
p O.c
def O.a = 10
p O.c

class K
  class << self
    attr_accessor :v
  end
end
def kv
  K.v = 3
  K.v
end
p kv
def K.v = 5
def K.v=(x)
  @v = x * 2
end
p kv

class R
  def self.x = 1
  def self.y = 2
  def self.t = x + y
end
p R.t
class R
  class << self
    attr_reader :x, :y
  end
end
R.instance_variable_set(:@x, 10)
R.instance_variable_set(:@y, 20)
p R.t
class R
  def self.x = 100
  def self.y = 200
end
p R.t
