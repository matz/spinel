# A class method's visibility that a method body's word sets, read where
# the receiver is not the constant itself: a boxed or nil-able value, a
# Class value a block or a parameter holds, and self in the class's own
# class methods.
class Gauge
  def self.level = 7
  def self.seal
    private_class_method :level
    nil
  end
  def self.unseal
    public_class_method :level
    nil
  end
  def self.asks = [respond_to?(:level), self.respond_to?(:level), respond_to?(:level, true)]
  def self.peek = (public_method(:level).call rescue $!.class)
end

class Dial
  def self.level = 1
end

def via(k) = (k.level rescue $!.class)
def report(tag)
  maybe = tag == :none ? nil : Gauge
  p [tag, [Gauge, 1][0].respond_to?(:level), defined?([Gauge, 1][0].level)]
  p [tag, maybe&.respond_to?(:level), defined?(maybe.level), Gauge.then { |k| (k.level rescue $!.class) }]
  p [tag, via(Gauge), via(Dial), Gauge.asks, Gauge.peek]
end

report(:start)
Gauge.seal
report(:sealed)
report(:none)
Gauge.unseal
report(:unsealed)

# A word that takes a def, as the last statement of a class body a proc
# runs: the def's version, then the word's visibility.
class Gauge
  def self.dial = :dial
end
seal_dial = proc do
  class Gauge
    private_class_method def self.dial = :dial2
  end
  nil
end
p [[Gauge, 1].map { |k| (k.dial rescue $!.class) }, Gauge.respond_to?(:dial)]
seal_dial.call
p [[Gauge, 1].map { |k| (k.dial rescue $!.class) }, Gauge.respond_to?(:dial), Gauge.send(:dial)]
