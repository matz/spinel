# Test: constructor call via ConstantPathNode (::C.new)

class C
  def initialize
    puts "init"
  end
end

puts "start"
::C.new
