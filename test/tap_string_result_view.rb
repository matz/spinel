def collect(values)
  [(puts :read; values[0]).tap { |value| puts value; GC.start }.to_s]
end

original = +"item"
result = collect([original])[0]
p result.equal?(original)
result << "!"
p original
