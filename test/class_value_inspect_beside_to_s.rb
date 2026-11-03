# Module#inspect is not Module#to_s: a class method to_s the program defines
# changes what to_s answers, a class value's inspect stays its name (and a
# subclass inherits both).

class Z
  def self.to_s = "zts"
end
class Y < Z; end

def texts(k) = [k.inspect, k.to_s]
p texts(Z), texts(Y), Z.inspect, Y.inspect, Z.to_s, Y.to_s
puts Z, Y
puts "#{Z} #{Y}"
