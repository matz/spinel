class Ring
  attr_reader :members, :data
  def initialize
    @members = []
    @data = {"seed" => "ok"}
  end
  def fill
    members[0] = self
    members.each { |obj| obj.data["late"] = [1] }
  end
  def late
    read_late
  end
  def read_late
    @data["late"]
  end
end
r = Ring.new
r.fill
v = r.late
if v.is_a?(Array)
  puts "array"
else
  puts "other"
end
