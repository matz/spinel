def invoke(&callback)
  callback.call { "kept" }
end

puts invoke { |&execute| execute = execute; reader = -> { execute }; reader.call.call }
puts invoke(&proc { |&execute| execute = execute; reader = -> { execute }; reader.call.call })

def without_block(&callback)
  callback.call
end
p without_block { |&execute| execute = execute; reader = -> { execute }; reader.call }

# Each call of the receiving block binds its own block for readers that outlive the call.
def invoke_twice(&callback)
  first = callback.call { "first" }
  second = callback.call { "second" }
  [first.call, second.call]
end
p invoke_twice { |&execute| execute = execute; -> { execute.call } }

def each_reader(&callback)
  out = []
  %w[a b c].each { |tag| out << callback.call { tag } }
  out.map(&:call)
end
p each_reader { |&execute| execute = execute; -> { execute.call } }
