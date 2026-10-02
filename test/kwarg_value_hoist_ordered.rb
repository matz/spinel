# A keyword argument whose value hoists a statement (Array#fetch inside a
# call) still runs after the keyword arguments to its left.

class Reader
  def initialize(values)
    @values = values
    @i = 0
  end

  def int
    v = @values[@i]
    @i += 1
    v
  end
end

NAMES = %i[none some].freeze

Pair = Data.define(:first_value, :second_value)

class Pair
  def self.read(input) = new(first_value: input.int, second_value: named(NAMES.fetch(input.int)))

  def self.named(name) = name == :none ? nil : name
end

p Pair.read(Reader.new([7, 1])).to_h

# Several keywords, each reading the next value, with a fetch as a direct
# keyword value.
VMODELS = %i[mos6569 mos8565].freeze
CMODELS = %i[mos6526 mos6526a].freeze
SIDS = %i[mos6581 mos8580].freeze
Setup = Data.define(:vic, :cia, :sid, :extra)

class Setup
  def self.read(input)
    new(vic: VMODELS.fetch(input.int), cia: CMODELS.fetch(input.int),
        sid: SIDS.fetch(input.int), extra: Pair.named(NAMES.fetch(input.int)))
  end
end

p Setup.read(Reader.new([0, 0, 1, 0])).to_h
