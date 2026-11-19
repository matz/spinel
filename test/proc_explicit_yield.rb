def invoke_string(&callback)
  callback.yield
end

value = +"answer"
answer = invoke_string { value }
puts answer.equal?(value)

callback = proc { |n, suffix:, &block| block.call(n) + suffix }
puts callback.yield(3, suffix: "!") { |n| n.to_s }
