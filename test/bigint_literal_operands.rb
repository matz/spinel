# spinel: int64
# Integer literals as operands of Bignum operations, and literal shifts past
# a word: each names one shared Bignum, and every use answers as before.
MASK = 0xFFFF_FFFF_FFFF_FFFF
SIGN = 0x8000_0000_0000_0000

def show(label, v)
  puts "#{label}: #{v.inspect}"
end

def wrap(v)
  v &= MASK
  v -= 1 << 64 if v & SIGN != 0
  v
end

big = MASK + 2
show "and", big & 0xFFFF
show "or", big | 1
show "xor", big ^ -1
show "and negative literal", big & -256
show "compare zero", big != 0
show "compare", big > 5
show "spaceship", big <=> 7
show "literal on the left", 7 <=> big
show "case equality", 0 === big
show "plus", big + 1
show "minus", big - 3
show "times", big * -2
show "shift past a word", 1 << 64
show "negative shift past a word", -3 << 70
show "shift that fits", 5 << 3
show "wrap negative", wrap(-5)
show "wrap positive", wrap(5)
show "wrap past", wrap(MASK + 10)

class Holder
  def initialize
    @v = nil
  end

  def fill
    @v ||= 0
    @v = MASK + 2 if @v == 7
    @v
  end
end
show "ivar or-write", Holder.new.fill

def bump(x = 0) = x + MASK
show "default argument", bump
show "given argument", bump(3)

first = second = 0
second = MASK + 1 if first == 99
show "chained write", [first, second]
show "word minimum", big & -9223372036854775808

@acc = 0
@acc = MASK + 1 if big > 0
3.times { |i| @acc += i }
show "ivar from zero", @acc

sum = 0
2000.times do |i|
  x = wrap(i * (MASK - 12345))
  sum ^= x & 0xFFFF
  sum += 1 if x != 0
end
show "loop", sum
