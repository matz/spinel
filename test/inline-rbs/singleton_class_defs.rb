# A def in `class << self` is a method of the class, as `def self.` makes one,
# as Active Support writes `Time.zone` and `Time.current`:
#
#   class Time
#     class << self
#       #: () -> (ActiveSupport::TimeZone | false | nil)
#       def zone
#
# Its signature applies as that of a `def self.`: the program compiled with
# `--no-inline-rbs --rbs sig/singleton_class_defs`, whose signatures are
# `def self.` ones, produces the same C byte for byte. (ruby/rbs reads such a
# def as an instance method's.) Each `untyped` slot pins what inference would
# not choose, so the C shows the signature applied.
class Clock
  class << self
    #: (untyped) -> untyped
    def echo(x)
      x
    end

    # @rbs (untyped) -> untyped
    def twice(x) = [x, x]

    #: (Integer, untyped) -> Integer
    def add(a, b)
      a + b.to_i
    end
  end

  #: (untyped) -> untyped
  def echo(x) = x
end

module Units
  class << self
    #: (untyped) -> untyped
    def wrap(x) = [x]
  end
end

p Clock.echo(1), Clock.twice(2), Clock.add(4, "5")
p Clock.new.echo(6), Units.wrap(7)
