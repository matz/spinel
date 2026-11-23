# A class method `<=>` of the program's orders class values: the runtime
# comparator of a no-block sort, min or max of classes reaches it ahead of
# Module's `<=>` (which answers nil for two unrelated classes), and so does a
# direct comparison of two typed classes.
class Railtie
  LOAD_ORDER = []

  class << self
    def load_index = LOAD_ORDER.index(name)

    def <=>(other)
      load_index <=> other.load_index
    end

    def inherited(subclass)
      LOAD_ORDER << subclass.name
      super
    end
  end
end

class Zeta < Railtie; end
class Alpha < Railtie; end
class Mid < Railtie; end

p [Mid, Alpha, Zeta].sort.map(&:name)
p [Zeta, Mid].min.name
p [Zeta, Mid, Alpha].max.name
p(Alpha <=> Zeta)
p(Zeta <=> Zeta)
p(Zeta < Railtie)
