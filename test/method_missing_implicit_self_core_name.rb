# A receiverless call inside a method_missing class goes to the hook when the
# class chain lacks the name, even if another class (Set#add) defines it.
require "set"
class Img
  def initialize(v) = @v = v
  def v = @v
  def +(o) = add(o)
  def method_missing(name, *args) = name == :add ? Img.new(@v + args[0].v.to_i) : super
  def respond_to_missing?(n, p = false) = n == :add
end
p (Img.new(2) + Img.new(3)).v
p Set[1].add(2).size
