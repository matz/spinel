# spinel: reject-string-minus
def options
  puts "argument"
  {amount: 2}
end
begin
  "payload".-(**options)
rescue NoMethodError => e
  p [e.class, e.name, e.receiver, e.args]
end
