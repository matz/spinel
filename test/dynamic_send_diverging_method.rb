class Gate
  def halt = throw(:halt)

  def fail_hard
    raise "boom"
  end

  def open = :opened
end

class Wall
  def halt = throw(:halt)
  def open = :solid
end

gate = Gate.new
[:halt, :fail_hard, :open].each do |m|
  r = begin
    catch(:halt) { gate.send(m) }
  rescue => e
    [e.class, e.message]
  end
  p [m, r]
end

[Gate.new, Wall.new].each do |o|
  [:halt, :open].each do |m|
    p [o.class, m, catch(:halt) { o.send(m) }]
  end
end
