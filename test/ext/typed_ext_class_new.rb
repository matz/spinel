# A typed --ext-entry and a method of the --ext-class table `new` the same
# class: the table's `new` forwarder, an instance method or a module method
# hands initialize the host's boxed arguments. initialize's `w` kept the
# typed entry's Integer through the fixpoint, and a late re-bind (pass's
# parameter, typed only then) widened it after @sizes had settled on the
# Integer-valued Hash, so the write was refused. The stores written from a
# parameter that late re-bind widens join their slots again now. ext-test
# checks the three tables compile.
module ExtTC
  class Shape
    def initialize(name, w)
      @sizes = { "w" => w }
    end
  end

  class Maker
    def make(a, b) = Shape.new(a, b)
  end

  #: (String, Integer) -> Shape
  def self.shape(name, w) = Shape.new(name, w)

  def self.mk(a, b) = Shape.new(a, b)

  def self.pass(lines) = lines.size

  def self.use = pass(["a"].map { |l| l })
end
