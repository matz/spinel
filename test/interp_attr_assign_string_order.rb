# An interpolation whose parts change a String an attribute assignment
# stores and a String another part holds: every part's String is the String
# itself, so the result shows the changes of all the parts, as CRuby reads
# each part after the last has run.
class Cell
  attr_accessor :v
end

St = Struct.new(:m)

class Run
  def initialize
    @s = +"s"
    @o = Cell.new
  end

  def attribute
    o = Cell.new
    o.v = +"a"
    s = +"s"
    r = "#{(o.v = +"q") << "!"}|#{s << "?"}"
    p [r, o.v, s]
    t = +"t"
    r2 = "#{t}|#{(o.v = t) << "!"}|#{t << "?"}"
    p [r2, o.v, t]
  end

  def holders
    o = Cell.new
    o.v = +"a"
    @s = +"s"
    $g = +"g"
    r1 = "#{(o.v = @s) << "!"}|#{@s << "?"}|#{o.v}"
    r2 = "#{$g}|#{(o.v = $g).upcase!}|#{$g.concat("c")}"
    r3 = "#{(o.v = +"n").replace("N")}|#{@s.replace("S")}"
    p [r1, r2, r3, o.v, @s, $g]
    @o.v = +"i"
    r4 = "#{(@o.v = @s) << "+"}|#{@s << "-"}"
    p [r4, @o.v, @s]
    st = St.new(+"m")
    s = +"x"
    r5 = "#{(st.m = s).concat("c", "d")}|#{s << "e"}"
    p [r5, st.m, s]
  end

  def chains
    o = Cell.new
    s = +"s"
    p ["#{(o.v = s) << "!" << "?"}|#{s << "1"}", o.v, s]
    p ["#{((o.v = s) << "a").to_s}|#{s << "2"}", o.v, s]
    p ["#{((o.v = s) << "b").itself}|#{s << "3"}", o.v, s]
    p ["#{((o.v = s) << "c").dup}|#{s << "4"}", o.v, s]
    p ["#{(o.v = s).<<("d")}|#{s.upcase!}", o.v, s]
    p ["#{(o&.v = s) << "e"}|#{s.replace("R")}", o.v, s]
    p ["#{(o.v = +"w") << "f"}|#{o.v << "g"}|#{o.v}", o.v]
  end

  def nesting
    o = Cell.new
    s = +"s"
    nested = "[#{(o.v = s) << "!"}|#{s << "?"}]-#{"<#{s}>"}"
    p [nested, o.v, s]
    heredoc = <<~TEXT
      #{(o.v = s) << "h"}|#{s << "i"}
    TEXT
    p [heredoc, o.v, s]
    p :"#{(o.v = s) << "y"}|#{s << "z"}"
    p [("#{(o.v = s) << "A"}|#{s << "B"}"), s]
  end
end

r = Run.new
r.attribute
r.holders
r.chains
r.nesting

AdjacentCell = Struct.new(:v)
s = +"a"
o = AdjacentCell.new
p ("#{(o.v = s) << "b"}|#{s << "c"}" "#{s.upcase!}")
p [s, o.v]
