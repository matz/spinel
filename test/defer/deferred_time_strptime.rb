# Compiled with --defer-refusals: a Time.strptime in a method, without
# `require "time"`, is deferred like any other refusal -- the method raises
# NotImplementedError naming it, and a program that never calls it builds
# and runs. Without the flag it is refused.
# spinel: defer-refusals: 2:start true after
def expires_at(s) = Time.strptime(s, "%Y-%m-%d")

puts "start"
begin
  expires_at("2024-02-29")
rescue NotImplementedError => e
  puts e.message.include?("Time.strptime is not supported")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
