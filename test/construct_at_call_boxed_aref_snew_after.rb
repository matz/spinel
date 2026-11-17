S = Struct.new(:a, :b)
vals = [S, [10, 20, 30]]
k = vals[ARGV.size]
p k[1, 2].to_a
class S
  def self.new(*a) = :own_new
end
p k[1, 2].to_a
p S.new(1, 2)
