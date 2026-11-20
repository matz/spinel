# A constant bound to a bare `Hash.new` or `Array.new` has no element type
# of its own. Read with no store into it, it was left untyped and taken for
# undefined: every reference raised NameError. Filled by `TABLE[k] = v`
# where another module also has a class of that leaf (Shapes::Table), its
# stores were not found under the constant's own key, the Hash was built as
# an Array and the C did not compile.
module Shapes
  class Table
    def initialize(x) = @x = x
  end
end
module Records
  module Index
    Table = Hash.new
    Table[:literal] = %i[literal].freeze
    All = Table.values.flatten.uniq.sort.freeze
    Kinds = Table.keys.freeze
    List = Array.new
    List << :first
  end
end
p Records::Index::All, Records::Index::Kinds, Records::Index::List
p Shapes::Table.new(1).class

EMPTY_HASH = Hash.new
EMPTY_LIST = Array.new
p EMPTY_HASH, EMPTY_HASH.size, EMPTY_LIST, EMPTY_LIST.empty?
module Records
  Counts = Hash.new(0)
  Counts[:a] += 1
  Table = {}
  Table[:b] = 2
end
p Records::Counts, Records::Table
