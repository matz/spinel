# A boxed Struct has no last, join or sample; a boxed IO has no length,
# empty?, last or join; a boxed Symbol has no include?. Each raises
# NoMethodError, as in CRuby. A Struct's own (or a module's) last and
# join still run, and its values_at, include? and length answer.
S1 = Struct.new(:a, :b)
class S2 < Struct.new(:a, :b)
  def last = :own_last
  def join(*) = "own_join"
end
module Lj
  def last = :mod_last
end
S3 = Struct.new(:a) do
  include Lj
end
vals = [S1.new(1, 2), S2.new(3, 4), S3.new(5), :sym, "str", [7, 8], (1..3), $stdout]
vals.each do |v|
  r = []
  r << (v.length rescue $!.class)
  r << (v.empty? rescue $!.class)
  r << (v.last rescue $!.class)
  r << (v.join rescue $!.class)
  r << (v.sample.class rescue $!.class)
  r << (v.values_at(0) rescue $!.class)
  r << (v.member?(1) rescue $!.class) unless v.equal?($stdout)
  r << (v.include?(1) rescue $!.class) unless v.equal?($stdout)
  puts r.map { |x| x.inspect.sub(/0x\h+/, "") }.join(" | ")
end
t = [Process.times, 1].first
p((t.last rescue $!.class), (t.join rescue $!.class), t.length)
v = [Struct.new(:a).new(1), 2].first
p((v.last rescue $!.class), (v.sample rescue $!.class), (v.join(",") rescue $!.class))
# an `at` alone still reaches the Struct check
def struct_at(x) = (x.at(0) rescue $!.class)
p struct_at([Struct.new(:q).new(1), 2].first)
