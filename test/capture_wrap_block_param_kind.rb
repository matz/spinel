# A block parameter that a proc in the body captures keeps the kind its
# caller hands it: the Array `[].tap` yields is written as an Array and the
# Hash `{}.tap` yields as a Hash, where an index write with a key of no
# settled type had decided the kind. The program's own `each`, yielding
# from a block, is what makes the inner blocks procs here.

class Slice
  def initialize(n) = @n = n

  def each
    return enum_for(:each) unless block_given?

    @n.times { |i| yield i }
  end
end

TABLE = [].tap do |table|
  "a".upto("c") { |c| table[c.ord] = [:id, c].freeze }
  %w[x y].each { |c| table[c.ord] = [:sym, c].freeze }
  table.freeze
end

p TABLE.size
p TABLE[98]
p TABLE[121]

INDEX = {}.tap do |index|
  %w[x y].each { |c| index[c.ord] = [:sym, c].freeze }
end

p INDEX
Slice.new(2).each { |i| p i }
