# Threads setting instance variables of names made at run time on one
# object each keep theirs: the object's table of such names is written
# under a lock, where two writes at once lost one or left a slot of the
# table's order unwritten (a name the program never set read back from
# instance_variables). Reads and an inspect beside the writes see whole
# tables too.
class C
  def initialize = @fixed = 0
end
obj = C.new

T = 8
N = 500
threads = T.times.map { |t|
  Thread.new {
    N.times { |i| obj.instance_variable_set("@v#{t}_#{i}", t * N + i) }
  }
}
reader = Thread.new {
  seen = 0
  200.times {
    seen = [seen, obj.instance_variables.size].max
    obj.inspect.size
    obj.instance_variable_get("@v0_0")
  }
  seen
}
threads.each(&:join)
p reader.value <= T * N + 1

names = obj.instance_variables
p names.size
p names.uniq.size
p names.all? { |n| n == :@fixed || n.to_s.start_with?("@v") }
p (0...T).all? { |t| (0...N).all? { |i| obj.instance_variable_get("@v#{t}_#{i}") == t * N + i } }

