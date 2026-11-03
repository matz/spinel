# A receiver that stays unknown still needs the conservative getter fallback.
class Ring
  attr_reader :members, :data
  def initialize
    @members = []
    @data = {"seed" => "ok"}
  end
  def fill
    members[0] = self
    members.each { |obj| obj.data[:late] = 42 }
  end
end
r = Ring.new
r.fill
p r.data
