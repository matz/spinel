# A class whose superclass is Hash: stores, reads, the Hash methods it does
# not define, dup and merge keeping the class, reflection
class Params < Hash
  alias_method :to_params_hash, :to_h
end
pr = Params.new
pr["a"] = 1
pr["b"] = [2]
pr["b"] << 3
p pr["a"], pr.keys, pr.size, pr.fetch("a"), pr.fetch("z", 0), pr.fetch("y") { |k| k * 2 }
p pr.to_params_hash, pr.to_params_hash.class, pr.class, pr.is_a?(Hash), Hash === pr
pr.update({ "c" => "x" }) { |k, o, n| n }
pr.each { |k, v| puts "#{k}=#{v.inspect}" }
pr.each { |pair| p pair }
p pr, pr.map { |k, v| k }, pr.select { |k, v| k == "a" }, pr.key?("c"), pr.include?("q")
p pr.sort_by { |k, v| k }.first, pr.count, pr.to_a, pr == { "a" => 1, "b" => [2, 3], "c" => "x" }
d = pr.dup
d["new"] = 9
p d.class, d.size, pr.size, pr.merge({ "m" => 1 }).class
defs = Params.new(0)
p defs["missing"]
class Counts < Hash
  def initialize
    super(0)
    @hits = 0
  end
  attr_reader :hits
  def bump(k)
    @hits += 1
    self[k] += 1
  end
end
c = Counts.new
c.bump(:x); c.bump(:x); c.bump(:y)
p c, c.hits, c[:z]

p Params.ancestors, Params.superclass, Params.instance_methods(false)
class Sym < Hash
end
s = Sym.new
s[:k] = 1
s[2] = "two"
p s, s.keys, s.class
