# spinel: int64
# Integer#succ, #next and #pred at the ends of the word answer as `+ 1` and
# `- 1` do in the build's --int-overflow mode (raise: RangeError, wrap: the
# wrapped value, promote: a Bignum), where they skipped the overflow check.
n = ARGV.size
x = 9223372036854775807 - n
y = -9223372036854775807 + n

def same(a, b)
  ra = begin; a.call; rescue RangeError; :range_error; end
  rb = begin; b.call; rescue RangeError; :range_error; end
  ra == rb
end

p same(-> { x.succ }, -> { x + 1 })
p same(-> { x.next }, -> { x + 1 })
p same(-> { (y - 1).pred }, -> { (y - 1) - 1 })
p same(-> { [x].map(&:succ) }, -> { [x + 1] })
p same(-> { [x].map { |v| v.succ } }, -> { [x + 1] })
p same(-> { x.pred.succ }, -> { x })
