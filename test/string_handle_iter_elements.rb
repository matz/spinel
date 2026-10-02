# Reading iterator elements and appending through a local Array still work.
a = [+"a"]
a.each { |x| x << "!" }
p a
begin
  "t".tap { |x| x << "!" }
rescue FrozenError
  puts "FrozenError"
end
