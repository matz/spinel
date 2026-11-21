# A Method read out of a Hash and called with a splat: every parameter
# takes the boxed arguments, the 17th and after too, so a Float there is
# not checked against an Integer slot (it raised TypeError)
class Table
  def initialize
    @entries = { "two" => method(:two), "many" => method(:many), "wide" => method(:wide) }
  end

  def dispatch(name, *args)
    @entries.fetch(name).call(*args)
  end

  def direct(name)
    @entries.fetch(name).call(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16.5)
  end

  def two(a, b)
    return b, a
  end

  def many(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16)
    return p16, p15, p2, p0
  end

  def wide(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17, p18, p19)
    [p0, p16, p17, p18, p19].map { |v| v.class }
  end
end

t = Table.new
p t.dispatch("two", 1, 2.0)
p t.dispatch("many", 0, 1, 2.0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16.0)
p t.dispatch("many", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)
p t.dispatch("wide", *Array.new(16) { |i| i }, 16.0, "s", :sym, nil)
p t.direct("many")
