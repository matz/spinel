class Ring
  attr_reader :members, :data
  def initialize
    @members = []
    @data = {"seed" => "ok"}
  end
  def fill(x)
    members[0] = self
    members.each { |obj| obj.data["late"] = x }
  end
  def late
    @data["late"]
  end
end
r = Ring.new
r.fill([])
v = r.late
if v.is_a?(Array)
  puts "array"
else
  puts "other"
end
