def invoke(&callback)
  callback.call(*(1..2).to_a) { "inner" }
end
p(invoke { |*values, &block| values })
