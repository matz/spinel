# spinel: reject-string-minus
def callback
  puts "block argument"
  -> { puts "body" }
end
begin
  "payload".-(2, &callback)
rescue NoMethodError => e
  p e.class
  p e.name
  p e.receiver
  p e.args
end
