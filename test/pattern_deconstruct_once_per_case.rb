$dk = 0; $d = 0; $ev = 0
class Shape
  attr_reader :x
  def initialize(x = 5) = (@x = x)
  def deconstruct_keys(keys) = ($dk += 1; {x: @x})
  def deconstruct = ($d += 1; [@x, @x + 1])
end
def shape(f) = f ? Shape.new : nil
def subj(f) = ($ev += 1; shape(f))
[true].each do |f|
  puts(case subj(f); in [1, 2] then "a"; in {x: 9} then "b"; in [a, b] then "c#{a}#{b}"; in {x:} then "d#{x}"; in nil then "n"; end)
  puts "ev=#{$ev} d=#{$d} dk=#{$dk}"
  puts(case subj(f); in [a, b] if a > 100 then "g"; in Shape => s if s then "cap"; in nil => z then "zn#{z.inspect}"; end)
  puts(case subj(f); when Shape then "w"; when nil then "wn"; end)
  y = case subj(f); when nil then "en"; when Shape then "es"; end; puts y
  puts "ev=#{$ev} d=#{$d} dk=#{$dk}"
  r = (subj(f) in {x: 5}); puts r
  r = (subj(f) in [5, 6]); puts r
  puts "ev=#{$ev} d=#{$d} dk=#{$dk}"
end
# a deconstruct that is an alias of an inherited method is cached too
class PairBase
  def to_pair = ($n += 1; [1, 2])
end
class PairAlias < PairBase
  alias deconstruct to_pair
end
$n = 0
r = case PairAlias.new
    in [1, 3] then :a
    in [1, 2] then :b
    end
p [r, $n]
# a deconstruct whose answer changes between calls: the case sees its first
# answer in every array arm
$n = 0
class Shifty
  def deconstruct = ($n += 1; $n == 1 ? [1, 3] : [1, 2])
end
r = begin
  case Shifty.new
  in [1, 2] then :a
  in [1, 3] then :b
  end
rescue NoMatchingPatternError
  :nomatch
end
p [r, $n]
