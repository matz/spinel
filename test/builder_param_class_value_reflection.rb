# self in a class body, held beside another value, is the class
class W
  def w = 1
  s = rand < 2 ? self : 5
  p s.instance_methods(false)
  p s.name
end

# A constant builder's block parameter, reassigned in the body, holds the
# class until then: a reflection on it answers for that class.
M = Module.new do |m|
  def mm = 1
  [1].each { p m.instance_methods(false) }
  m = "m"
  p m
end

X = Class.new do |k|
  def a = 1
  [1].each { p k.instance_methods(false) }
  [1].each { p k.const_defined?(:Q) }
  k = 5
  p k + 1
end
