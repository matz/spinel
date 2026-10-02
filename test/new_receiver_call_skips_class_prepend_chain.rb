# Follow-up to #6879: `K.new.m` reaches K#m only, so it does not make live the
# shadowed copy in a class method's super chain (two extended modules both
# defining m, the later one calling super). Color's class-side Base#set could
# only raise (self[:r] on the class); no call reaches it, so it stays dead
# rather than being refused.
module Base
  def set(v)
    self[:r] = v
    self
  end
end

module Logged
  def set(v)
    super
  end
end

class Color
  extend Base
  extend Logged
  def initialize; @h = {}; end
  def []=(k, v); @h[k] = v; end
  def [](k); @h[k]; end
  def set(r); self[:r] = r; self; end
end

# A class-side chain that is called still runs through its super.
module Named
  def label = "named"
end

module Loud
  def label = super.upcase
end

class Tag
  extend Named
  extend Loud
  def label = "instance"
end

p Color.new.set(7)[:r]
p Tag.new.label
p Tag.label
