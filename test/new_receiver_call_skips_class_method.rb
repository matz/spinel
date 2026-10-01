# `K.new.m` answers an instance of K, so it reaches K#m and never `def self.m`:
# a class method of the same name that no call reaches stays dead, even when
# its body could only raise (raylib's Color.set, which writes self[:r] on the
# class).
class Color
  def initialize; @h = {}; end
  def []=(k, v); @h[k] = v; end
  def [](k); @h[k]; end
  def set(r); self[:r] = r; self; end
  def self.set(rgba)
    self[:r] = rgba
    self
  end
  def shade = self[:r]
  alias tone shade
end

class Plain
  def self.label = "class"
  def label = "instance"
end

module Util
  module_function
  def twice(x) = x * 2
end

class Holder
  include Util
  def run = twice(21)
end

p Color.new.set(7)[:r]
p Color.new.set(9).tone
p Plain.new.label
p Plain.label
p Holder.new.run
