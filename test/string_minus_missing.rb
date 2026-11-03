# The local/local missing String operator keeps its receiver and arguments.
seen = []
def left(seen)
  seen << "left"
  String.new("bytes")
end
def right(seen)
  seen << "right"
  2
end
def subtract(value, amount)
  value - amount
end
value = left(seen)
amount = right(seen)
begin
  subtract(value, amount)
rescue NoMethodError => e
  p [e.class, e.name, e.receiver.equal?(value), e.receiver, e.args]
end
p seen
