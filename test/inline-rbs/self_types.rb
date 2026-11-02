# `self` and `self?` in a class's instance methods, and `top` as a type
# argument, as Active Support writes `squish!`, `presence` and `truncate`:
#
#   #: () -> self
#   #: () -> self?
#   #: (Integer, ?Hash[Symbol, top]) -> String
#
# `self` is an instance of the class the method is written in, as its name
# would say; `Hash[Symbol, top]` is a Hash of Symbols to values of any type,
# as `Hash[Symbol, untyped]` is. The `untyped` slot beside them pins what
# inference would not choose, so the C shows the signature applied, and the
# program compiled with `--no-inline-rbs --rbs sig/self_types` produces the
# same C byte for byte.
class String
  #: (untyped) -> self
  def tidy!(x)
    strip!
    self
  end

  #: (untyped) -> self?
  def filled(x) = empty? ? nil : self

  #: (Integer, ?Hash[Symbol, top]) -> untyped
  def cut(n, opts = {}) = self[0, n] + opts.fetch(:tail, "").to_s
end

class Counter
  attr_reader :n

  def initialize = @n = 0

  #: (untyped) -> self
  def bump(by)
    @n += by.to_i
    self
  end

  #: (untyped) -> self?
  def positive(x) = @n > 0 ? self : nil
end

class Tally < Counter; end

p " a b ".dup.tidy!(1), "".filled(nil), "x".filled(:y)
p "hello".cut(2), "hello".cut(3, tail: "..."), "hello".cut(1, tail: 5)
p Counter.new.bump(1).bump("2").n, Counter.new.positive(0)
t = Tally.new.bump(3)
p t.class, t.positive(0).class
