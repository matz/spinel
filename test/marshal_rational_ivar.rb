# An object whose ivar holds a Rational goes through Marshal and comes back
# with the same Rational, 0 and a negative one included.
class Share
  def initialize(part, name)
    @part = part
    @name = name
  end

  attr_reader :part, :name
end

[Rational(1, 3), Rational(0), Rational(-7, 2)].each do |r|
  s = Marshal.load(Marshal.dump(Share.new(r, "x")))
  p [s.part, s.name, s.part == r]
end
