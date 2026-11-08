# A program's own methods named like the compiler's ENV rows (__env_value,
# __env_to_h) are no builtin to the share analysis either: no builtin's
# contract answers for a call of the name, so an each over its answer
# still drops the block's value, and the identity Set the block ends with
# keeps the identity check to that Set's own calls. The plain Set holding a
# String is then not checked. (No top-level or Object method of the names
# here: one would leave the proof for any name.)
class KrBase
  def initialize = @seen = Set.new.compare_by_identity
  def run_v
    __env_value.each { |x| @seen.add(x); @seen }
    @seen.size
  end
  def run_h
    __env_to_h.each { |x| @seen.add(x); @seen }
    @seen.size
  end
end
class KrA < KrBase
  def __env_value = [:a, :b]
  def __env_to_h = [:c]
end
class KrB < KrBase
  def __env_value = {a: :b}
  def __env_to_h = {c: :d}
end
p KrA.new.run_v, KrB.new.run_v, KrA.new.run_h, KrB.new.run_h
plain = Set.new
plain << +"str"
p plain.include?("str")
ENV["SPK_KRS1"] = "v"
p ENV.to_h["SPK_KRS1"]
