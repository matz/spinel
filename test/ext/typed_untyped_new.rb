# Two --ext-entry methods `new` the same class: one typed from inline RBS,
# one with no types. The untyped entry is called from C, so its boxed
# arguments reach initialize as they do from a called method, and @sizes
# takes the Hash initialize builds from them. Left as an uncalled method's,
# initialize's `w` kept the typed entry's Integer until a late re-bind
# (pass's parameter, typed only then) widened it, after @sizes had settled
# on the Integer-valued Hash, and the write was refused. ext-test checks it
# compiles.
module ExtTU
  class Shape
    def initialize(name, w)
      @sizes = { "w" => w }
    end
  end

  #: (String, Integer) -> Shape
  def self.shape(name, w) = Shape.new(name, w)

  def self.mk(a, b) = Shape.new(a, b)

  def self.pass(lines) = lines.size

  def self.use = pass(["a"].map { |l| l })
end
