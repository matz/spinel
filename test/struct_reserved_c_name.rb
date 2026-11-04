# A Struct named after one of the runtime's own C types (Mutex, SumState,
# OpenStruct, ...) has a mangled C name (u_Mutex). The Struct's #inspect and
# #to_s, its #to_a and other emitters named its C type and functions after the
# Ruby name instead, so the C did not compile. CRuby warns on stderr that
# Mutex is already initialized; stdout is what this compares.
Mutex = Struct.new(:a, :b)
SumState = Struct.new(:a, :b)
EqlSet = Struct.new(:a)
StringLiteral = Struct.new(:s)
OpenStruct = Struct.new(:a, :b)
module App
  Queue = Struct.new(:a, :b)
  Monitor = Struct.new(:a)
end
# a String member named to_s answers #to_s
Named = Struct.new(:to_s, :n)

def show(s)
  puts s.to_s
  p s, s.to_a, s.to_h
end
show(Mutex.new(1, 2))
show(SumState.new(3, 4))
show(EqlSet.new(5))
show(StringLiteral.new("lit"))
show(OpenStruct.new(6, 7))
show(App::Queue.new(8, 9))
show(App::Monitor.new(10))
boxed = [Mutex.new(1, 2), SumState.new(3, 4), OpenStruct.new(5, 6), 0][ARGV.size]
puts boxed.to_s
p boxed.inspect, boxed.to_a
case boxed
in [a, b] then p a + b
end
p Mutex.new(1, 2) == Mutex.new(1, 2), SumState.new(1, 2).eql?(SumState.new(1, 2))
n = Named.new("custom", 1)
puts n.to_s
puts "#{n}"
