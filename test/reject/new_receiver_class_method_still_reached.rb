# The class method IS called here, so its provable NoMethodError is reported.
class Color
  def []=(k, v); end
  def set(r); self[:r] = r; self; end
  def self.set(rgba)
    self[:r] = rgba
    self
  end
end
Color.new.set(1)
Color.set(2)
