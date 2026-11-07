# count, first, last, include?, join and sample on a boxed builtin that has
# none of them raise NoMethodError, as in CRuby.

def pick(i) = [Time.at(0), /re/, Object.new, ->(x) { x * 2 }, Rational(1, 2), Mutex.new,
               StandardError.new("e"), Random.new(1), [1, 2], "ab", 1..3][i]
11.times do |i|
  v = pick(i)
  r = [(v.count rescue $!.class), (v.first rescue $!.class), (v.last rescue $!.class),
       (v.include?(1) rescue $!.class), (v.join rescue $!.class),
       (v.sample.class rescue $!.class)]
  p r
end

# A builtin that has the method keeps it: Proc#[], Thread#join, and a Dir's
# count (Dir includes Enumerable).
def pick2(i) = [->(x) { x * 2 }, Thread.new { 5 }, Dir.new("."), [3]][i]
p pick2(0)[4]
t = pick2(1)
p t.join.class
d = pick2(2)
p d.count > 0
p pick2(3).first
