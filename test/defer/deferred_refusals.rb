# Compiled with --defer-refusals: a refused method compiles to a raise
# of NotImplementedError naming the refusal, and so does a refused
# statement at top level: the rest of the program builds, and a run
# stops where it reaches a refused line instead of going on without it.
# Without the flag the same program is refused.
# spinel: defer-refusals: 2:top NotImplementedError true done
def normalize(s) = proc { s }.ruby2_keywords

puts "top"
begin
  normalize("a")
rescue NotImplementedError => e
  puts e.class
  puts e.message.include?("ruby2_keywords")
end
puts "done"
proc { "a" }.ruby2_keywords
puts "not reached"
