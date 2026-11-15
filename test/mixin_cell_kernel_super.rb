# A class method that includes Kernel into a blank slate gives its instances Kernel's
# methods from then on: a super and a bare respond_to_missing? take Kernel's answer where the
# include ran, and NoMethodError where it did not.
class Ran < BasicObject
  def self.mix = include(::Kernel)
  mix
  def to_s = super
  def fmt = respond_to_missing?(:x, false)
end

class Idle < BasicObject
  def self.mix = include(::Kernel)
  mix if ::ARGV.size == 3
  def to_s = super
  def fmt = respond_to_missing?(:x, false)
end

class Later < BasicObject
  def to_s = super
  def self.mix = include(::Kernel)
  def fmt = respond_to_missing?(:x, false)
end

def attempt
  r = yield
  p r
rescue NoMethodError
  puts "NoMethodError"
end

attempt { Ran.new.to_s.start_with?("#<Ran") }
attempt { Ran.new.fmt }
attempt { Idle.new.to_s.start_with?("#<Idle") }
attempt { Idle.new.fmt }
attempt { Later.new.to_s.start_with?("#<Later") }
Later.mix
attempt { Later.new.to_s.start_with?("#<Later") }
attempt { Later.new.fmt }
