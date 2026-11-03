# spinel: reject-string-minus
def text
  puts "receiver"
  "payload"
end
def amount
  puts "argument"
  2
end
begin
  text - amount
rescue NoMethodError => e
  p e.class
  p e.name
  p e.receiver
  p e.args
end
