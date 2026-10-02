# The class-side super chain IS called here (Color.set), so the shadowed
# Base#set's provable NoMethodError on the class is reported.
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
  def []=(k, v); end
  def set(r); self[:r] = r; self; end
end
Color.new.set(1)
Color.set(2)
