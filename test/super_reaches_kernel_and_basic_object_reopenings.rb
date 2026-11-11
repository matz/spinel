# A super that no class of the program answers reaches a method the program
# defines on Kernel, then on BasicObject, unless CRuby's own Object or Kernel
# has a method of the name first.
class BasicObject
  def itself = :basic_object
  def depth_name(x) = :basic_object
  def itself_name = :basic_object
end
module Kernel
  def itself_name = :kernel
end
class A
  def itself = super
  def depth_name(x) = super
  def itself_name = super
  def ==(o) = super
end
p A.new.depth_name(1)
p A.new.itself_name
p A.new == 1
p A.new.itself.class
