def invoke
  yield(*(1..2).to_a)
end
invoke { |*values, &block| p values }
