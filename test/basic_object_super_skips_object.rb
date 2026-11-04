# A class under BasicObject has neither Object nor Kernel among its
# ancestors: a super in its method does not reach a method the program
# gives Object or Kernel, and raises NoMethodError, as in CRuby. With
# `include ::Kernel` the Kernel method is its ancestor's, and a plain
# class's super still reaches Object's.
class Object; def zed = "oz"; end
module Kernel; def kz = "kz"; end
class BO5 < BasicObject
  def zed = super
  def kz = super
  def args(x) = super(x)
end
class BO6 < BasicObject
  include ::Kernel
  def kz = super
end
class BO7 < BO5; def zed = super; end
class Plain; def zed = super; end
[-> { BO5.new.zed }, -> { BO5.new.kz }, -> { BO5.new.args(1) }, -> { BO6.new.kz },
 -> { BO7.new.zed }, -> { Plain.new.zed }].each do |l|
  begin
    p l.call
  rescue NoMethodError
    puts "NoMethodError"
  end
end
# An include the body makes through a constant alias or inside a block
# can bring Kernel in: such a class keeps reaching Kernel's method.
KAlias = Kernel
class BO8 < BasicObject; include ::KAlias; def kz = super; end
class BO9 < BasicObject
  [1].each { include ::Kernel }
  def kz = super
end
p BO8.new.kz
p BO9.new.kz
