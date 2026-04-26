# Test: class defined inside module and instantiated through M::C.new

module M
  class C
    def initialize
      puts "init"
    end

    def step
      puts "step"
    end
  end
end

puts "start"
obj = M::C.new
obj.step
