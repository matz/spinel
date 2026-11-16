# A module of the program that includes Kernel (the program never reopens Kernel) brings
# Kernel into a blank slate where an include of it runs: under a condition in the body, or
# in a class method. Where the include did not run the class has no Kernel.
module KM; include ::Kernel; end

class CondRan < BasicObject
  include ::KM if ::ARGV.size == 0
  def to_s = super
end
class CondIdle < BasicObject
  include ::KM if ::ARGV.size == 3
  def to_s = super
end
class ViaMethodRan < BasicObject
  def self.mix = include(::KM)
  mix
  def to_s = super
end
class ViaMethodIdle < BasicObject
  def self.mix = include(::KM)
  mix if ::ARGV.size == 3
  def to_s = super
end

def attempt
  p yield
rescue NoMethodError
  puts "NoMethodError"
end

attempt { CondRan.new.to_s.start_with?("#<CondRan") }
attempt { CondIdle.new.to_s.start_with?("#<CondIdle") }
attempt { ViaMethodRan.new.to_s.start_with?("#<ViaMethodRan") }
attempt { ViaMethodIdle.new.to_s.start_with?("#<ViaMethodIdle") }
