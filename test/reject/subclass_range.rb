# A subclass of Range took none of Range's constructor arguments (#7075).
class Span < Range
end

p Span.new(1, 5).to_a
