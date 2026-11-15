def values
  puts "values"
  [1, 2]
end
def invoke(&callback)
  callback.call(*values) { "inner" }
end
invoke { |first, second = (puts "default"; 3), &block| p [first, second]; puts block.call }
