# spinel: reject-string-minus
value = String.new("payload")
amount = 2
def callback
  puts "block argument"
  -> { puts "body" }
end
begin
  value.-(amount, &callback)
rescue NoMethodError => e
  p [e.receiver, e.args]
end
