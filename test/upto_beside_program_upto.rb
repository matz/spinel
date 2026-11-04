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
