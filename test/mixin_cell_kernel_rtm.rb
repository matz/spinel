# A blank slate's conditional respond_to_missing? def has Kernel's behind it where an include that
# may not have run brought Kernel in (the include's own statement sets the class's cell), and
# NoMethodError where it did not.
class ViaMethod < BasicObject
  if ::ARGV.size == 9
    def respond_to_missing?(n, p) = true
  end
  def t = respond_to_missing?(:x, false)
  def self.mix = include(::Kernel)
  mix
end

class ViaMethodIdle < BasicObject
  if ::ARGV.size == 9
    def respond_to_missing?(n, p) = true
  end
  def t = respond_to_missing?(:x, false)
  def self.mix = include(::Kernel)
  mix if ::ARGV.size == 3
end

class ViaBody < BasicObject
  include ::Kernel if ::ARGV.size == 3
  if ::ARGV.size == 9
    def respond_to_missing?(n, p) = true
  end
  def t = respond_to_missing?(:x, false)
  def to_s = super
end

class ViaBodyRan < BasicObject
  include ::Kernel if ::ARGV.size == 0
  def to_s = super
end

def attempt
  p yield
rescue NoMethodError
  puts "NoMethodError"
end

attempt { ViaMethod.new.t }
attempt { ViaMethodIdle.new.t }
attempt { ViaBody.new.t }
attempt { ViaBody.new.to_s.start_with?("#<ViaBody") }
attempt { ViaBodyRan.new.to_s.start_with?("#<ViaBodyRan") }
