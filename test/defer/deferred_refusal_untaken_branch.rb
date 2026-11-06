# Compiled with --defer-refusals: a refused statement in a method raises
# NotImplementedError where it stands, not at the method's entry, so a
# refusal in a branch that never runs leaves the rest of its method
# working. A run that takes the branch stops there.
# spinel: defer-refusals: 3:a b a NotImplementedError true a b 1 :done a b 1 2 NotImplementedError done
class Server
  def initialize(config)
    puts "a"
    if config[:Listen]
      warn("deprecated", uplevel: 1)
      puts "not reached"
    end
    puts "b" unless config[:Listen]
  end

  def each_line(xs)
    xs.each do |x|
      p x
      proc { "a" }.ruby2_keywords if x == 2
    end
    :done
  end
end

Server.new({Port: 1})
begin
  Server.new({Listen: []})
rescue NotImplementedError => e
  puts e.class
  puts e.message.include?("uplevel")
end
p Server.new({}).each_line([1])
begin
  Server.new({}).each_line([1, 2])
rescue NotImplementedError => e
  puts e.class
end
puts "done"
proc { "a" }.ruby2_keywords
puts "not reached"
