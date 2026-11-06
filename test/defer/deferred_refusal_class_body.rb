# Compiled with --defer-refusals: a refused statement in a class body
# raises NotImplementedError where it stands, so the body's later lines
# do not run as if it had not been there.
# spinel: defer-refusals: 1:before
class Thing
  puts "before"
  proc { "a" }.ruby2_keywords
  puts "not reached"
end
puts "not reached"
