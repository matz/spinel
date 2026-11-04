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
    @data["late"]
  end
end
r = Ring.new
r.fill
v = r.late
case v
when Array
  puts "array"
when String
  puts "string"
when NilClass
  puts "nil"
else
  puts "other"
end
