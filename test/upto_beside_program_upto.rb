# A blockless Integer#upto / #downto (a Bignum's too) and String#upto on a
# value of a slot a program class's own `upto` shares the name with. Under
# --int-overflow=promote an Integer sum is boxed, and the dispatch the
# program's `upto` built had no arm for it: NoMethodError (tzinfo's
# `(year + 1).upto(n)` beside the date package's Date#upto).
class Span
  def upto(x)
    [:span, x]
  end

  def downto(x)
    [:span_down, x]
  end
end

y = ARGV.size + 2020
p y.upto(y + 2).to_a
p (y + 1).upto(y + 2).flat_map { |v| [v, v] }
p (y + 3).downto(y).to_a
p y.upto(y + 1)
big = 2**64 + ARGV.size
p big.upto(big + 1).to_a
p (big + 1).downto(big).to_a
p "a".upto("c").to_a
p Span.new.upto(1), Span.new.downto(2)
# A boxed Bignum's walk is read as far as it goes, not laid out (toward an
# infinite Float it never ends), and its size counts the members between.
boxed = [2**64, "s"][ARGV.size]
p boxed.upto(Float::INFINITY).first(2)
p boxed.upto(2**64 + 2).size, boxed.downto(2**64 - 2).size, boxed.upto(2**64 - 1).size
p boxed.upto(2**64 + 1.5).to_a
# A String's walk has no size of its own.
p "a".upto("c").size
# A Float limit counts in Floats, as CRuby counts it (a rounded receiver
# included), and its count is the Integer it is, however far off.
near = [2**64 + 1, "s"][ARGV.size]
p near.upto((2**64).to_f).size, near.upto((2**64).to_f).to_a
down = [2**64, "s"][ARGV.size]
p down.downto(1.8446744073709552e19).size
p [-2**1023, "s"][ARGV.size].upto(Float::MAX).first(1).size
