# A class-side accessor a later def of its name replaces is the method
# until that def runs, and the def from then on: a call reaches the one
# live where it runs, and a call in a method body that runs on both sides
# reaches each in turn. A call that runs once, before a later def of its
# name is made, runs none of that def's body: the def's own definitions
# come at a later call.

class W
  class << self
    attr_accessor :cfg
  end
end
W.cfg = 2
p W.cfg
class W
  def self.cfg=(v)
    @cfg = v * 10
  end
end
W.cfg = 3
p W.cfg

class R
  class << self
    attr_reader :cfg
  end
end
R.instance_variable_set(:@cfg, 2)
p R.cfg
class R
  def self.cfg
    :redef
  end
end
p R.cfg

class S
  class << self
    attr_writer :cfg
    def cfg=(v)
      @cfg = v * 10
    end
  end
end
S.cfg = 2
p S.instance_variable_get(:@cfg)

class V
  class << self
    attr_accessor :cfg
  end
end
k = [V][0]
k.cfg = 2
p V.cfg
class V
  def self.cfg=(v)
    @cfg = v * 10
  end
end
k.cfg = 3
p V.cfg

def set_t(v) = (T.cfg = v)
class T
  class << self
    attr_accessor :cfg
  end
end
set_t(2)
p T.cfg
class T
  def self.cfg=(v)
    @cfg = v * 10
  end
end
set_t(3)
p T.cfg

class U
  class << self
    attr_accessor :cfg
  end
end
U.cfg = 1
p U.cfg
class U
  def self.cfg=(v)
    @cfg = -v
  end
end
U.cfg = 5
p U.cfg

class Q
  def self.a = 1
end
Q.a
def Q.a = (def Q.k = :w)
p Q.respond_to?(:k)
Q.a
p Q.respond_to?(:k), Q.k
2.times do
  Q.a
  def Q.a = (def Q.j = :v)
end
p Q.respond_to?(:j)

# The accessor's slot and the def's @x are one class-level variable.
class Z
  class << self
    attr_accessor :z
  end
end
Z.instance_variable_set(:@z, 4)
p Z.z
class Z
  def self.z=(v)
    nil
  end
end
Z.z = 9
p Z.z, Z.instance_variable_get(:@z)

class P
  class << self
    attr_reader :p1
  end
  @p1 = 3
end
p P.p1
class P
  def self.p1 = @p1 * 2
end
p P.p1, P.instance_variable_get(:@p1)

# A def that names no @x: the accessor's slot is still the class-level
# variable's, read before the def.
class N
  class << self
    attr_reader :x
  end
end
p N.x
class N
  def self.x = :d
end
p N.x

# A Method of the replaced accessor answers the accessor's parameters,
# which have no names: a writer's one parameter, a reader's none.
class Pm
  class << self
    attr_accessor :x
  end
end
pw = Pm.method(:x=)
pr = Pm.method(:x)
class Pm
  def self.x=(v)
    @x = v * 10
  end
  def self.x = :redef
end
p pw.parameters, pr.parameters, pw.inspect.sub(/ \S+:\d+>\z/, ">")
p [pw][0].parameters, pw.unbind.parameters
pw.call(4)
p pr.call, Pm.x
