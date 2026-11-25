# Queries on instances of classes made at run time read the class each was
# built for: instance_of? and is_a? against a superclass constant, respond_to?,
# method_missing, and a typed parameter.
class Shape
  def area = 0
  def method_missing(name, *args) = name == :ghost ? :boo : super
  def respond_to_missing?(name, priv = false) = name == :ghost || super
end

def square = Class.new(Shape) { def area = 4 }

def exact(s) = s.instance_of?(Shape)

k = square
s = k.new
p s.instance_of?(Shape), s.is_a?(Shape), s.instance_of?(k), s.area
p exact(Shape.new), exact(s)
p s.respond_to?(:ghost), s.ghost, s.respond_to?(:area)
begin
  s.missing
rescue NoMethodError
  p :no_method
end
j = square
p j.new.instance_of?(k), j.new.instance_of?(j), k.new.area + j.new.area
