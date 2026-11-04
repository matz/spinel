# A program that reaches its end has no $! in its hooks, whatever it rescued on
# the way there: an exit that was rescued ended nothing.
at_exit { puts "hook: #{$!.inspect}" }
begin
  raise "handled"
rescue
  puts "in a rescue: #{$!.inspect}"
end
begin
  exit 3
rescue SystemExit => e
  puts "rescued #{e.status}: #{$!.inspect}"
end
puts "end: #{$!.inspect}"
