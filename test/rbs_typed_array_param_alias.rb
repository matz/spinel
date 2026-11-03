# An empty `[]` handed to a parameter an inline RBS signature declares as a
# typed Array or Hash is the caller's own object: the callee's writes land in
# it, not in a converted copy.

module M
  #: (String, Array[String]) -> void
  def self.add(item, acc)
    acc << item
    nil
  end

  #: (String, String, Hash[String, String]) -> void
  def self.put(k, v, acc)
    acc[k] = v
    nil
  end

  def self.run
    acc = []
    add("a", acc)
    add("b", acc)
    acc.join(",")
  end

  def self.run_block
    acc = []
    "d e".split(" ").each { |w| add(w, acc) }
    acc.join(",")
  end

  def self.run_hash
    acc = {}
    put("k", "v", acc)
    put("x", "y", acc)
    acc.map { |k, v| "#{k}=#{v}" }.join(",")
  end
end

class Collector
  #: (Integer, Array[Integer]) -> void
  def add(n, acc)
    acc << n
    nil
  end

  def run
    acc = []
    add(1, acc)
    add(2, acc)
    acc.sum
  end
end

puts M.run
puts M.run_block
puts M.run_hash
puts Collector.new.run
