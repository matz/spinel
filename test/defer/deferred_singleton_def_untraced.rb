# Compiled with --defer-refusals: a singleton method on a String, which
# spinel cannot give a method of its own, is refused where the def runs, so
# the method defining it raises NotImplementedError when it runs rather than
# answering String's own `==` (minitest's long_invisible assertion test).
# Without the flag it is refused.
# spinel: defer-refusals: 2:start true after
def long_invisible
  exp = "blah" * 10
  def exp.==(_) = false
  exp == "blah" * 10
end

puts "start"
begin
  long_invisible
rescue NotImplementedError => e
  puts e.message.include?("cannot trace to one `new`")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
