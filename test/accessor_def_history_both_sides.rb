# A class with an accessor on each side of one name, and a def that redefines
# the instance side later: each instance call answers the version live at it
# (the accessor before the def, the def after), and the class side's accessor
# stays a plain slot. The class side's reader used to take the instance side's
# accessor version for its own slot stand-in, so no call reached it.
class Both
  class << self
    attr_accessor :x
  end
  attr_accessor :x
  def initialize = @x = :inst0
end
Both.x = :cls1
b = Both.new
b.x = :inst1
p Both.x, b.x
b.x = :inst2
Both.x = :cls2
p Both.x, b.x
class Both
  def x = [:inst_def, @x]
end
p Both.x, b.x
b.x = :inst3
p b.x, Both.x
class Both
  def self.x = [:cls_def, @x]
  def self.x=(v)
    @x = [:w, v]
  end
end
Both.x = :cls3
p Both.x, b.x
p Both.new.x

# the same through `+=`, with the instance writer redefined last
class Pair
  attr_accessor :x
  class << self
    attr_accessor :x
  end
  def initialize = @x = 1
end
Pair.x = 10
q = Pair.new
q.x += 1
Pair.x += 5
p Pair.x, q.x
class Pair
  def self.x = @x * 2
end
class Pair
  def x=(v)
    @x = v + 100
  end
end
q.x = 2
Pair.x = 3
p Pair.x, q.x

# the accessor read before the def, and a class-side reader of the name
class Early
  class << self
    attr_reader :x
  end
  attr_accessor :x
end
e = Early.new
e.x = :inst1
p e.x
class Early
  def x = [:inst_def, @x]
end
p e.x
