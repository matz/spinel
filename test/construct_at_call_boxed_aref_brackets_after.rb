S = Struct.new(:a, :b)
vals = [S, [10, 20, 30]]
k = vals[ARGV.size]
p k[1, 2]
class S
  def self.[](*a) = :own_brackets
end
p k[1, 2]
